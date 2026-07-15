/**
 * @file runtime.cppm
 * @brief Vulkan/MoltenVK runtime for the Phase A G3SA tiled-DP chaining kernel.
 *
 * Owns the Vulkan instance / physical device / logical device / compute queue,
 * the fixed chaining descriptor-set + pipeline layout, a per-tile-size compute
 * pipeline cache, and one reusable command buffer + fence. Exposes exactly one
 * hot entry point — `run_chaining_tiled_dp_roundtrip` — which records every
 * H2D copy, the dispatch, and every D2H copy under a single queue lock.
 *
 * Device selection, feature probing, and all per-run failures are runtime-side
 * decisions: init failures set `features_.available == false` with an
 * initialization error, and per-run failures throw `VulkanRuntimeError`. Phase A
 * targets unified memory (Apple Silicon / MoltenVK): device-local buffers come
 * back host-visible+coherent, so the round-trip writes/reads them directly
 * without a staging bounce (DOC0304 §4.1).
 */
module;
#include "common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <dlfcn.h>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime;

export import bioseq.gpu.vulkan.shader_cache;
export import bioseq.gpu.vulkan.shader_layout;
export import bioseq.gpu.vulkan.shader_variants;
export import bioseq.gpu.vulkan.phase_b_abi;
export import bioseq.gpu.vulkan.phase_c_abi;
import bioseq.common.mlogging;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.instr;  // DOC0310 §21 — phase_c_* sub-region timing labels

export namespace bioseq::gpu::vulkan {

class Runtime;

struct AlignmentCigarBatchResult {
    std::vector<GpuAlignmentCigarResultWire> results{};
    std::vector<std::uint32_t> packed_cigar{};
};

/// Capabilities probed once at init. `available == false` means there is no
/// usable Vulkan device and selected GPU algorithms must fail.
struct VulkanFeatureSet {
    bool available = false;
    bool shader_int64 = false;
    bool scalar_block_layout = false;
    bool timeline_semaphore = false;
    bool software_device = false;
    std::uint32_t device_type = 0;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t max_workgroup_invocations = 0;
    std::uint32_t max_workgroup_size_x = 0;
    std::uint32_t max_workgroup_size_y = 0;
    std::uint32_t max_workgroup_size_z = 0;
    std::uint32_t max_workgroup_count_x = 0;
    std::uint32_t max_shared_memory_bytes = 0;
    std::uint64_t max_storage_buffer_range = 0;
    std::uint64_t min_storage_buffer_offset_alignment = 1;
    std::uint64_t device_local_bytes = 0;
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::array<char, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE> device_name{};
};

/// Canonical allocation-free Vulkan driver identity used by SQL/report proof.
/// Numeric fields avoid unstable marketing strings and preserve the exact
/// pipeline-cache compatibility UUID byte order reported by Vulkan.
[[nodiscard]] inline std::array<char, 128>
canonical_vulkan_driver_identity(const VulkanFeatureSet& features) noexcept {
    std::array<char, 128> result{};
    constexpr char hex[] = "0123456789abcdef";
    std::size_t cursor = 0u;
    const auto append_literal = [&result, &cursor](std::string_view value) {
        if (cursor > result.size() - value.size() - 1u) return false;
        for (const char ch : value) result[cursor++] = ch;
        return true;
    };
    const auto append_u32 = [&result, &cursor](std::uint32_t value) {
        constexpr char digits[] = "0123456789abcdef";
        if (cursor > result.size() - 9u) return false;
        for (std::uint32_t shift = 28u;; shift -= 4u) {
            result[cursor++] = digits[(value >> shift) & 0x0fu];
            if (shift == 0u) break;
        }
        return true;
    };
    if (!append_literal("api=") || !append_u32(features.api_version)
        || !append_literal(";driver=")
        || !append_u32(features.driver_version)
        || !append_literal(";vendor=") || !append_u32(features.vendor_id)
        || !append_literal(";device=") || !append_u32(features.device_id)
        || !append_literal(";pcuuid=")) {
        return {};
    }
    for (const std::uint8_t byte : features.pipeline_cache_uuid) {
        if (cursor > result.size() - 3u) return {};
        result[cursor++] = hex[byte >> 4u];
        result[cursor++] = hex[byte & 0x0fu];
    }
    result[cursor] = '\0';
    return result;
}

/// RAII owner for one (VkBuffer, VkDeviceMemory) pair. Move-only; destruction
/// routes through `Runtime::destroy_buffer` (the single cleanup path).
struct Buffer {
    Buffer() = default;
    Buffer(Runtime* owner, VkBuffer buffer, VkDeviceMemory memory,
           std::uint64_t bytes, std::uint64_t allocation_bytes,
           VkMemoryPropertyFlags memory_properties,
           bool process_footprint_reflected,
           void* mapped) noexcept
        : owner(owner), buffer(buffer), memory(memory), bytes(bytes),
          allocation_bytes(allocation_bytes),
          memory_properties(memory_properties),
          process_footprint_reflected(process_footprint_reflected),
          mapped(mapped) {}
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept { move_from(other); }
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }
    ~Buffer() { reset(); }

    void reset() noexcept;
    [[nodiscard]] bool valid() const noexcept { return buffer != VK_NULL_HANDLE; }
    [[nodiscard]] bool host_visible_coherent() const noexcept {
        constexpr VkMemoryPropertyFlags required =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        return (memory_properties & required) == required;
    }

    Runtime* owner = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint64_t bytes = 0;
    std::uint64_t allocation_bytes = 0;
    VkMemoryPropertyFlags memory_properties = 0;
    bool process_footprint_reflected = false;
    void* mapped = nullptr;

private:
    void move_from(Buffer& other) noexcept {
        owner = other.owner;
        buffer = other.buffer;
        memory = other.memory;
        bytes = other.bytes;
        allocation_bytes = other.allocation_bytes;
        memory_properties = other.memory_properties;
        process_footprint_reflected = other.process_footprint_reflected;
        mapped = other.mapped;
        other.owner = nullptr;
        other.buffer = VK_NULL_HANDLE;
        other.memory = VK_NULL_HANDLE;
        other.bytes = 0;
        other.allocation_bytes = 0;
        other.memory_properties = 0;
        other.process_footprint_reflected = false;
        other.mapped = nullptr;
    }
};

template <class Handle>
[[nodiscard]] inline std::uint64_t vulkan_handle_identity(
    const Handle handle) noexcept {
    static_assert(sizeof(Handle) <= sizeof(std::uint64_t));
    std::uint64_t identity = 0u;
    std::memcpy(&identity, &handle, sizeof(handle));
    return identity;
}

/// Immutable snapshot of one Runtime-owned allocation.  The object address is
/// insufficient because Buffer is move-assignable; operation proof therefore
/// binds its owner, Vulkan allocation handles, and physical/logical sizes too.
struct RuntimeBufferIdentity {
    const Buffer* object = nullptr;
    const Runtime* owner = nullptr;
    std::uint64_t buffer_handle = 0u;
    std::uint64_t memory_handle = 0u;
    std::uint64_t bytes = 0u;
    std::uint64_t allocation_bytes = 0u;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return object != nullptr && owner != nullptr
            && buffer_handle != 0u && memory_handle != 0u
            && bytes != 0u && allocation_bytes >= bytes;
    }
    constexpr bool operator==(const RuntimeBufferIdentity&) const noexcept =
        default;
};

[[nodiscard]] inline RuntimeBufferIdentity runtime_buffer_identity(
    const Buffer* buffer) noexcept {
    if (buffer == nullptr) return {};
    return RuntimeBufferIdentity{
        .object = buffer,
        .owner = buffer->owner,
        .buffer_handle = vulkan_handle_identity(buffer->buffer),
        .memory_handle = vulkan_handle_identity(buffer->memory),
        .bytes = buffer->bytes,
        .allocation_bytes = buffer->allocation_bytes,
    };
}

/// One independent hard chaining range = one dispatched workgroup.
struct GpuChainRange {
    std::uint32_t begin = 0;
    std::uint32_t length = 0;
};

/// Thrown on any per-run Vulkan failure once the device is usable. Selected
/// Vulkan algorithms must propagate this rather than silently using CPU work.
class VulkanRuntimeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Optional supervisor-owned provider for precompiled MoltenVK metallibs.
/// When installed, runtime pipeline creation asks this hook for a shader module
/// instead of invoking glslang/spirv-cross on the backend worker thread.
struct RuntimeShaderProvider {
    using PrepareFn = std::expected<void, std::string> (*)();
    using LoadFn = std::expected<ShaderModuleLoad, std::string> (*)(
        VkDevice device, const ShaderVariantKey& key, PFN_vkCreateShaderModule create_shader_module);
    PrepareFn prepare_runtime = nullptr;
    LoadFn load_module = nullptr;
};

RuntimeShaderProvider*& runtime_shader_provider_slot() noexcept {
    static RuntimeShaderProvider* provider = nullptr;
    return provider;
}

void install_runtime_shader_provider(RuntimeShaderProvider* provider) noexcept {
    runtime_shader_provider_slot() = provider;
}

[[nodiscard]] inline bool accepted_runtime_cache_digest(
    std::string_view digest) noexcept {
    return digest.size() == 16u
        && std::all_of(digest.begin(), digest.end(), [](char value) {
               return (value >= '0' && value <= '9')
                   || (value >= 'a' && value <= 'f');
           })
        && std::any_of(digest.begin(), digest.end(), [](char value) {
               return value != '0';
           });
}

/// One exact Metal binary archive inside the process-owned sealed seed image.
/// The ordinal binds runtime staging to retained bytes rather than a cache
/// pathname that could be replaced after supervisor validation.
struct RuntimeMetalArchiveSpec {
    std::uint32_t artifact_ordinal =
        (std::numeric_limits<std::uint32_t>::max)();
    std::string artifact_filename;
    std::uint64_t size = 0u;
    std::array<std::uint8_t, 32> sha256{};
    std::string negative_error_domain;
    std::int64_t negative_error_code = 0;

    [[nodiscard]] bool accepted() const noexcept {
        return artifact_ordinal
                != (std::numeric_limits<std::uint32_t>::max)()
            && !artifact_filename.empty() && size != 0u
            && !negative_error_domain.empty()
            && negative_error_domain.size() <= 1024u
            && negative_error_domain.find('\0') == std::string::npos
            && std::any_of(sha256.begin(), sha256.end(), [](std::uint8_t byte) {
                   return byte != 0u;
               });
    }
    [[nodiscard]] bool operator==(const RuntimeMetalArchiveSpec&) const noexcept = default;
};

/// Immutable input installed by the PostgreSQL cache owner before Runtime is
/// constructed.  It intentionally contains no directory: an unlisted archive
/// can never become eligible merely by appearing beside a listed one.
struct RuntimeMetalArchiveConfiguration {
    std::shared_ptr<const SealedShaderCacheSeedImage> seed_image;
    /// Optional external expectations.  Zero means Runtime must seal the
    /// selected Vulkan/Metal identities before vkCreateDevice; a nonzero value
    /// is an additional equality constraint, never a value to trust blindly.
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid{};
    std::uint64_t metal_device_registry_id = 0u;
    std::uint32_t qualification_version = 0u;
    std::uint64_t qualification_receipt_count = 0u;
    std::uint32_t helper_protocol_version = 0u;
    std::string helper_build_identity;
    std::string helper_source;
    std::uint64_t helper_executable_size = 0u;
    std::array<std::uint8_t, 32> helper_executable_sha256{};
    std::uint64_t helper_metal_registry_id = 0u;
    std::vector<RuntimeMetalArchiveSpec> archives;

    [[nodiscard]] bool accepted() const noexcept {
        if (!seed_image || !seed_image->accepted() || archives.empty()
            || qualification_version != 2u
            || qualification_receipt_count != archives.size()
            || helper_protocol_version != 2u
            || helper_build_identity != "bioseq_gpu_archive_builder_v2"
            || helper_source != "installed-sibling"
            || helper_executable_size == 0u
            || helper_metal_registry_id == 0u
            || std::all_of(
                helper_executable_sha256.begin(),
                helper_executable_sha256.end(),
                [](std::uint8_t value) { return value == 0u; })
            || (metal_device_registry_id != 0u
                && metal_device_registry_id != helper_metal_registry_id)
            || !accepted_runtime_cache_digest(
                seed_image->manifest().cache_digest)) {
            return false;
        }
        try {
        for (std::size_t i = 0u; i < archives.size(); ++i) {
            const RuntimeMetalArchiveSpec& archive = archives[i];
            const SealedShaderCacheArtifactImage* image =
                seed_image->artifact(archive.artifact_ordinal);
            if (!archive.accepted() || image == nullptr
                || image->descriptor.kind
                    != SealedShaderCacheArtifactKind::metal_binary_archive
                || image->descriptor.filename != archive.artifact_filename
                || image->descriptor.size != archive.size
                || image->descriptor.sha256 != archive.sha256
                || (i != 0u
                    && archives[i - 1u].artifact_ordinal
                        >= archive.artifact_ordinal)
                || (i != 0u
                    && (archive.negative_error_domain
                            != archives[0].negative_error_domain
                        || archive.negative_error_code
                            != archives[0].negative_error_code))) {
                return false;
            }
            const auto* receipt_image =
                sealed_shader_cache_detail::find_artifact_image(
                    seed_image->artifacts(),
                    SealedShaderCacheArtifactKind::
                        metal_binary_archive_qualification,
                    image->descriptor.key_index,
                    image->descriptor.filename + ".qualification");
            if (receipt_image == nullptr) return false;
            const auto receipt = sealed_shader_cache_detail::
                parse_metal_archive_qualification_receipt(
                    receipt_image->bytes);
            if (!receipt || !receipt->production_accepted()
                || receipt->archive_size != archive.size
                || receipt->archive_sha256 != archive.sha256
                || receipt->negative_error.domain
                    != archive.negative_error_domain
                || receipt->negative_error.code != archive.negative_error_code
                || receipt->wire_version != qualification_version
                || receipt->helper_protocol_version
                    != helper_protocol_version
                || receipt->helper_build_identity != helper_build_identity
                || receipt->helper_source != helper_source
                || receipt->helper_executable_size != helper_executable_size
                || receipt->helper_executable_sha256
                    != helper_executable_sha256
                || receipt->helper_metal_registry_id
                    != helper_metal_registry_id) {
                return false;
            }
        }
        } catch (...) {
            return false;
        }
        return true;
    }
    [[nodiscard]] bool operator==(
        const RuntimeMetalArchiveConfiguration&) const noexcept = default;
};

/// Single admitted process owner for installed archive configuration metadata.
/// The public configuration is a transient installation candidate; its vector
/// and strings are moved into this image without a second allocation or copy.
class RuntimeMetalArchiveConfigurationImage {
public:
    RuntimeMetalArchiveConfigurationImage(
        const RuntimeMetalArchiveConfigurationImage&) = delete;
    RuntimeMetalArchiveConfigurationImage& operator=(
        const RuntimeMetalArchiveConfigurationImage&) = delete;
    RuntimeMetalArchiveConfigurationImage(
        RuntimeMetalArchiveConfigurationImage&&) noexcept = default;
    RuntimeMetalArchiveConfigurationImage& operator=(
        RuntimeMetalArchiveConfigurationImage&&) noexcept = delete;

    [[nodiscard]] const RuntimeMetalArchiveConfiguration& configuration()
        const noexcept {
        return configuration_;
    }
    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        return retained_capacity_bytes_;
    }
    [[nodiscard]] std::uint64_t peak_admitted_bytes() const noexcept {
        return peak_admitted_bytes_;
    }
    [[nodiscard]] bool accepted() const noexcept {
        return configuration_.accepted() && permit_
            && retained_capacity_bytes_ != 0u
            && retained_capacity_bytes_ <= peak_admitted_bytes_
            && permit_.bytes() == retained_capacity_bytes_;
    }

private:
    RuntimeMetalArchiveConfigurationImage(
        RuntimeMetalArchiveConfiguration configuration,
        std::uint64_t peak_admitted_bytes,
        bioseq::parallelism::MemoryPermit permit) noexcept
        : permit_(std::move(permit)),
          configuration_(std::move(configuration)),
          peak_admitted_bytes_(peak_admitted_bytes) {}

    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::parallelism::saturating_add;
        using bioseq::parallelism::saturating_mul;
        std::uint64_t owned = sizeof(RuntimeMetalArchiveConfigurationImage);
        owned = saturating_add(
            owned,
            saturating_mul(
                static_cast<std::uint64_t>(configuration_.archives.capacity()),
                sizeof(RuntimeMetalArchiveSpec)));
        for (const RuntimeMetalArchiveSpec& archive : configuration_.archives) {
            owned = saturating_add(
                owned,
                static_cast<std::uint64_t>(
                    archive.artifact_filename.capacity()));
            owned = saturating_add(
                owned,
                static_cast<std::uint64_t>(
                    archive.negative_error_domain.capacity()));
        }
        owned = saturating_add(
            owned,
            static_cast<std::uint64_t>(
                configuration_.helper_build_identity.capacity()));
        owned = saturating_add(
            owned,
            static_cast<std::uint64_t>(
                configuration_.helper_source.capacity()));
        return owned;
    }

    [[nodiscard]] bool reconcile() noexcept {
        const std::uint64_t retained = owned_capacity_bytes();
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || !permit_ || retained > permit_.bytes()) {
            return false;
        }
        permit_.shrink_to(retained);
        retained_capacity_bytes_ = retained;
        return true;
    }

    // Reverse destruction releases strings/vector before their permit.
    bioseq::parallelism::MemoryPermit permit_;
    RuntimeMetalArchiveConfiguration configuration_;
    std::uint64_t peak_admitted_bytes_ = 0u;
    std::uint64_t retained_capacity_bytes_ = 0u;

    friend std::expected<void, std::string>
    install_runtime_metal_archive_configuration(
        std::shared_ptr<const RuntimeMetalArchiveConfigurationImage>);
    friend std::expected<std::shared_ptr<
        const RuntimeMetalArchiveConfigurationImage>, std::string>
    make_runtime_metal_archive_configuration_image(
        std::shared_ptr<const SealedShaderCacheSeedImage>,
        std::array<std::uint8_t, VK_UUID_SIZE>,
        std::uint64_t);
};

struct RuntimeMetalArchiveLiveGateEvidence {
    std::uint64_t install_validation_count = 0u;
    std::uint64_t live_gate_calls = 0u;
    std::uint64_t live_configuration_scan_count = 0u;
    bool installed_accepted = false;
};

struct RuntimePrivateArchiveSnapshotTestEvidence {
    std::filesystem::path snapshot_path;
    std::uint64_t streamed_bytes = 0u;
    std::uint64_t scratch_capacity_bytes = 0u;
    bool exact_seed_compared = false;
    bool tamper_rejected = false;
};

[[nodiscard]] RuntimeMetalArchiveLiveGateEvidence
runtime_metal_archive_live_gate_evidence() noexcept;

void probe_immutable_metal_policy_live_for_test();

[[nodiscard]] std::expected<RuntimePrivateArchiveSnapshotTestEvidence, std::string>
verify_runtime_private_archive_snapshot_for_test(
    std::string_view artifact_filename,
    std::span<const std::byte> bytes,
    const std::filesystem::path& temp_parent);

struct RuntimeMetalArchiveSetIdentity {
    std::uint64_t archive_count = 0u;
    std::uint64_t total_size = 0u;
    std::uint64_t ordered_checksum = 0u;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return archive_count != 0u && total_size != 0u
            && ordered_checksum != 0u;
    }
    [[nodiscard]] constexpr bool operator==(
        const RuntimeMetalArchiveSetIdentity&) const noexcept = default;
};

struct RuntimeVulkanDeviceIdentity {
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};

    [[nodiscard]] bool accepted() const noexcept {
        return api_version != 0u && driver_version != 0u
            && std::any_of(
                device_uuid.begin(), device_uuid.end(),
                [](std::uint8_t value) { return value != 0u; })
            && std::any_of(
                pipeline_cache_uuid.begin(), pipeline_cache_uuid.end(),
                [](std::uint8_t value) { return value != 0u; });
    }
    [[nodiscard]] bool operator==(
        const RuntimeVulkanDeviceIdentity&) const noexcept = default;
};

struct RuntimeMetalArchiveProcessQualification;
[[nodiscard]] std::uint64_t
runtime_metal_archive_process_qualification_identity(
    const RuntimeMetalArchiveProcessQualification&) noexcept;

/// Immutable process qualification created once while exact seed bytes are
/// installed into live Metal archive objects. Pipeline scopes borrow this
/// record; they never reopen or hash an archive pathname.
struct RuntimeMetalArchiveProcessQualification {
    std::uint32_t version = 0u;
    std::uint64_t identity = 0u;
    RuntimeMetalArchiveSetIdentity archive_set{};
    std::uint64_t receipt_count = 0u;
    std::uint64_t hash_passes = 0u;
    std::uint64_t hashed_bytes = 0u;
    std::uint64_t private_files_created = 0u;
    std::uint64_t private_files_unlinked = 0u;
    std::uint64_t live_private_paths = 0u;
    std::uint64_t pipeline_filesystem_hash_passes = 0u;
    std::uint64_t pipeline_filesystem_hashed_bytes = 0u;
    std::uint32_t helper_protocol_version = 0u;
    std::string_view helper_build_identity;
    std::string_view helper_source;
    std::uint64_t helper_executable_size = 0u;
    std::array<std::uint8_t, 32> helper_executable_sha256{};
    std::uint64_t helper_metal_registry_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::uint64_t seed_receipt_identity = 0u;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    bool saturated = true;

    [[nodiscard]] bool accepted() const noexcept {
        return version == 2u && identity != 0u
            && identity
                == runtime_metal_archive_process_qualification_identity(*this)
            && archive_set.accepted()
            && receipt_count == archive_set.archive_count
            && hash_passes == 1u && hashed_bytes == archive_set.total_size
            && private_files_created == archive_set.archive_count
            && private_files_unlinked == private_files_created
            && live_private_paths == 0u
            && pipeline_filesystem_hash_passes == 0u
            && pipeline_filesystem_hashed_bytes == 0u
            && helper_protocol_version == 2u
            && helper_build_identity == "bioseq_gpu_archive_builder_v2"
            && helper_source == "installed-sibling"
            && helper_executable_size != 0u
            && std::any_of(
                helper_executable_sha256.begin(),
                helper_executable_sha256.end(),
                [](std::uint8_t value) { return value != 0u; })
            && helper_metal_registry_id != 0u
            && RuntimeVulkanDeviceIdentity{
                .device_uuid = vulkan_device_uuid,
                .api_version = api_version,
                .driver_version = driver_version,
                .vendor_id = vendor_id,
                .device_id = device_id,
                .pipeline_cache_uuid = pipeline_cache_uuid,
            }.accepted()
            && seed_receipt_identity != 0u && seed_size != 0u
            && manifest_checksum != 0u
            && std::any_of(
                seed_sha256.begin(), seed_sha256.end(),
                [](std::uint8_t value) { return value != 0u; })
            && !saturated;
    }
    [[nodiscard]] bool operator==(
        const RuntimeMetalArchiveProcessQualification&) const noexcept = default;
};

[[nodiscard]] inline std::uint64_t
runtime_metal_archive_process_qualification_identity(
    const RuntimeMetalArchiveProcessQualification& value) noexcept
{
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    std::uint64_t identity = fnv_offset;
    const auto append_byte = [&identity](std::uint8_t byte) {
        identity ^= byte;
        identity *= fnv_prime;
    };
    const auto append_u64 = [&append_byte](std::uint64_t number) {
        for (std::uint32_t shift = 0u; shift != 64u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(number >> shift));
        }
    };
    const auto append_text = [&append_byte](std::string_view text) {
        for (const unsigned char byte : text) append_byte(byte);
        append_byte(0u);
    };
    append_u64(value.version);
    append_u64(value.archive_set.archive_count);
    append_u64(value.archive_set.total_size);
    append_u64(value.archive_set.ordered_checksum);
    append_u64(value.receipt_count);
    append_u64(value.hash_passes);
    append_u64(value.hashed_bytes);
    append_u64(value.private_files_created);
    append_u64(value.private_files_unlinked);
    append_u64(value.live_private_paths);
    append_u64(value.pipeline_filesystem_hash_passes);
    append_u64(value.pipeline_filesystem_hashed_bytes);
    append_u64(value.helper_protocol_version);
    append_text(value.helper_build_identity);
    append_text(value.helper_source);
    append_u64(value.helper_executable_size);
    for (const std::uint8_t byte : value.helper_executable_sha256) {
        append_byte(byte);
    }
    append_u64(value.helper_metal_registry_id);
    for (const std::uint8_t byte : value.vulkan_device_uuid) append_byte(byte);
    append_u64(value.api_version);
    append_u64(value.driver_version);
    append_u64(value.vendor_id);
    append_u64(value.device_id);
    for (const std::uint8_t byte : value.pipeline_cache_uuid) append_byte(byte);
    append_u64(value.seed_receipt_identity);
    for (const std::uint8_t byte : value.seed_sha256) append_byte(byte);
    append_u64(value.seed_size);
    append_u64(value.manifest_checksum);
    append_u64(value.saturated ? 1u : 0u);
    return identity == 0u ? fnv_offset : identity;
}

[[nodiscard]] inline RuntimeMetalArchiveSetIdentity
runtime_metal_archive_set_identity(
    std::span<const RuntimeMetalArchiveSpec> archives) noexcept {
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    std::uint64_t checksum = fnv_offset;
    std::uint64_t total_size = 0u;
    const auto append_byte = [&checksum](const std::uint8_t byte) {
        checksum ^= byte;
        checksum *= fnv_prime;
    };
    for (const RuntimeMetalArchiveSpec& archive : archives) {
        for (std::uint32_t shift = 0u; shift != 32u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(
                archive.artifact_ordinal >> shift));
        }
        for (const unsigned char byte : archive.artifact_filename) {
            append_byte(byte);
        }
        append_byte(0u);
        for (std::uint32_t shift = 0u; shift != 64u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(archive.size >> shift));
        }
        for (const std::uint8_t byte : archive.sha256) append_byte(byte);
        for (const unsigned char byte : archive.negative_error_domain) {
            append_byte(byte);
        }
        append_byte(0u);
        const std::uint64_t error_code_bits =
            std::bit_cast<std::uint64_t>(archive.negative_error_code);
        for (std::uint32_t shift = 0u; shift != 64u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(error_code_bits >> shift));
        }
        if (total_size > (std::numeric_limits<std::uint64_t>::max)()
                - archive.size) {
            total_size = (std::numeric_limits<std::uint64_t>::max)();
        } else {
            total_size += archive.size;
        }
    }
    return RuntimeMetalArchiveSetIdentity{
        .archive_count = static_cast<std::uint64_t>(archives.size()),
        .total_size = total_size,
        .ordered_checksum = checksum,
    };
}

/// Exact facts captured for one vkCreateComputePipelines call.  A receipt is
/// accepted only when every Metal pipeline call made under its explicit scope
/// replaced the descriptor archive set, forced fail-on-miss, and first proved
/// that the same descriptor misses against a fresh empty archive.
struct RuntimeMetalArchivePipelineAudit {
    std::uint64_t receipt_identity = 0u;
    std::uint64_t scope_identity = 0u;
    std::string cache_digest;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    std::uint64_t seed_receipt_identity = 0u;
    std::uint64_t seed_load_count = 0u;
    std::uint64_t seed_manifest_read_count = 0u;
    std::uint64_t seed_artifact_read_count = 0u;
    std::uint64_t seed_manifest_bytes = 0u;
    std::uint64_t seed_artifact_bytes = 0u;
    std::uint64_t seed_load_ns = 0u;
    std::uint64_t seed_admission_count = 0u;
    std::uint64_t seed_peak_admitted_bytes = 0u;
    std::uint64_t seed_retained_capacity_bytes = 0u;
    bool seed_admission_saturated = true;
    RuntimeMetalArchiveProcessQualification cache_qualification{};
    ShaderVariantKey shader_key{};
    std::string expected_entry_name;
    std::string observed_entry_name;
    std::uint64_t matched_descriptor_functions = 0u;
    RuntimeMetalArchiveSetIdentity expected_archives{};
    RuntimeMetalArchiveSetIdentity loaded_archives{};
    std::uint64_t archive_pre_post_identity_checks = 0u;
    std::uint64_t pipeline_pre_identity_checks = 0u;
    std::uint64_t pipeline_post_identity_checks = 0u;
    std::uint64_t unsealed_metal_source_requests = 0u;
    std::uint64_t metal_source_fallback_calls = 0u;
    bool command_interpose_installed = false;
    bool command_interpose_immutable_fail_closed = false;
    std::uint64_t command_loader_metal_registry_id = 0u;
    std::uint64_t command_source_methods_present = 0u;
    std::uint64_t command_source_wrapper_methods = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> configured_vulkan_device_uuid{};
    std::array<std::uint8_t, VK_UUID_SIZE> runtime_vulkan_device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::uint64_t pipeline_create_ns = 0u;
    std::uint64_t expected_metal_registry_id = 0u;
    std::uint64_t loader_metal_registry_id = 0u;
    std::uint64_t intercepted_metal_registry_id = 0u;
    std::uint64_t scoped_interceptions = 0u;
    std::uint64_t descriptor_replacements = 0u;
    std::uint64_t fail_on_miss_enforcements = 0u;
    std::uint64_t same_descriptor_negative_controls = 0u;
    std::uint64_t negative_control_misses = 0u;
    std::string expected_negative_error_domain;
    std::int64_t expected_negative_error_code = 0;
    std::string observed_negative_error_domain;
    std::int64_t observed_negative_error_code = 0;
    std::uint64_t original_sync_negative_calls = 0u;
    std::uint64_t actual_sync_calls = 0u;
    std::uint64_t actual_async_calls = 0u;
    std::uint64_t outside_scope_interceptions = 0u;
    bool nested_scope = false;
    bool sticky_failure = false;
    bool outer_vulkan_pipeline_succeeded = false;

    [[nodiscard]] bool accepted() const noexcept {
        const std::uint64_t actual_calls = actual_sync_calls + actual_async_calls;
        return receipt_identity != 0u && scope_identity != 0u
            && accepted_runtime_cache_digest(cache_digest)
            && std::any_of(
                seed_sha256.begin(), seed_sha256.end(),
                [](std::uint8_t byte) { return byte != 0u; })
            && seed_size != 0u && manifest_checksum != 0u
            && seed_receipt_identity != 0u && seed_load_count == 1u
            && seed_manifest_read_count == 1u
            && seed_artifact_read_count != 0u
            && seed_manifest_bytes != 0u
            && seed_artifact_bytes == seed_size && seed_load_ns != 0u
            && seed_admission_count == 1u
            && seed_peak_admitted_bytes != 0u
            && seed_retained_capacity_bytes != 0u
            && seed_retained_capacity_bytes <= seed_peak_admitted_bytes
            && !seed_admission_saturated
            && cache_qualification.accepted()
            && cache_qualification.archive_set == expected_archives
            && cache_qualification.seed_receipt_identity
                == seed_receipt_identity
            && cache_qualification.seed_sha256 == seed_sha256
            && cache_qualification.seed_size == seed_size
            && cache_qualification.manifest_checksum == manifest_checksum
            && valid_shader_variant_key(shader_key)
            && !expected_entry_name.empty()
            && observed_entry_name == expected_entry_name
            && matched_descriptor_functions == 1u
            && expected_archives.accepted()
            && expected_archives == loaded_archives
            && archive_pre_post_identity_checks
                == expected_archives.archive_count
            && pipeline_pre_identity_checks == 0u
            && pipeline_post_identity_checks == 0u
            && unsealed_metal_source_requests == 0u
            && metal_source_fallback_calls == 0u
            && command_interpose_installed
            && command_interpose_immutable_fail_closed
            && configured_vulkan_device_uuid == runtime_vulkan_device_uuid
            && std::any_of(
                runtime_vulkan_device_uuid.begin(),
                runtime_vulkan_device_uuid.end(),
                [](std::uint8_t byte) { return byte != 0u; })
            && api_version != 0u && driver_version != 0u
            && std::any_of(
                pipeline_cache_uuid.begin(), pipeline_cache_uuid.end(),
                [](std::uint8_t byte) { return byte != 0u; })
            && pipeline_create_ns != 0u
            && cache_qualification.helper_metal_registry_id
                == expected_metal_registry_id
            && cache_qualification.vulkan_device_uuid
                == runtime_vulkan_device_uuid
            && cache_qualification.api_version == api_version
            && cache_qualification.driver_version == driver_version
            && cache_qualification.vendor_id == vendor_id
            && cache_qualification.device_id == device_id
            && cache_qualification.pipeline_cache_uuid
                == pipeline_cache_uuid
            && expected_metal_registry_id != 0u
            && command_loader_metal_registry_id
                == expected_metal_registry_id
            && command_source_methods_present == 2u
            && command_source_wrapper_methods
                == command_source_methods_present
            && loader_metal_registry_id == expected_metal_registry_id
            && intercepted_metal_registry_id == expected_metal_registry_id
            && scoped_interceptions == 1u
            && actual_calls == scoped_interceptions
            && descriptor_replacements == scoped_interceptions
            && fail_on_miss_enforcements == scoped_interceptions
            && same_descriptor_negative_controls == scoped_interceptions
            && negative_control_misses == scoped_interceptions
            && !expected_negative_error_domain.empty()
            && observed_negative_error_domain
                == expected_negative_error_domain
            && observed_negative_error_code == expected_negative_error_code
            && original_sync_negative_calls == scoped_interceptions
            && outside_scope_interceptions == 0u
            && !nested_scope && !sticky_failure
            && outer_vulkan_pipeline_succeeded;
    }
};

namespace runtime_detail {
class RuntimeGpuCacheMetricsClaimPlan;
}

/// Move-only immutable proof retained by the canonical Vulkan pipeline cache
/// entry.  Cache views share that same object; they cannot mint another proof.
class RuntimeMetalArchivePipelineReceipt {
public:
    RuntimeMetalArchivePipelineReceipt() = default;
    RuntimeMetalArchivePipelineReceipt(
        const RuntimeMetalArchivePipelineReceipt&) = delete;
    RuntimeMetalArchivePipelineReceipt& operator=(
        const RuntimeMetalArchivePipelineReceipt&) = delete;
    RuntimeMetalArchivePipelineReceipt(
        RuntimeMetalArchivePipelineReceipt&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          metrics_claimed_(std::exchange(
              other.metrics_claimed_, false)) {}
    RuntimeMetalArchivePipelineReceipt& operator=(
        RuntimeMetalArchivePipelineReceipt&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            metrics_claimed_ = std::exchange(
                other.metrics_claimed_, false);
        }
        return *this;
    }

    [[nodiscard]] bool accepted() const noexcept { return audit_.accepted(); }
    [[nodiscard]] const RuntimeMetalArchivePipelineAudit& audit() const noexcept {
        return audit_;
    }

    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::parallelism::saturating_add;
        std::uint64_t owned = sizeof(RuntimeMetalArchivePipelineReceipt);
        owned = saturating_add(owned, audit_.cache_digest.capacity());
        owned = saturating_add(owned, audit_.expected_entry_name.capacity());
        owned = saturating_add(owned, audit_.observed_entry_name.capacity());
        owned = saturating_add(
            owned, audit_.expected_negative_error_domain.capacity());
        owned = saturating_add(
            owned, audit_.observed_negative_error_domain.capacity());
        return owned;
    }

    /// Pure contract hook: proves move invalidation and that multiple cache
    /// views retain one accepted receipt object rather than cloning evidence.
    [[nodiscard]] static bool self_test_move_and_reuse_contract(
        RuntimeMetalArchivePipelineAudit audit) {
        RuntimeMetalArchivePipelineReceipt issued{std::move(audit)};
        if (!issued.accepted()) return false;
        RuntimeMetalArchivePipelineReceipt moved{std::move(issued)};
        if (issued.accepted() || !moved.accepted()) return false;
        auto retained = std::make_shared<const RuntimeMetalArchivePipelineReceipt>(
            std::move(moved));
        const auto normal_cache_view = retained;
        const auto phase_b_cache_view = retained;
        return retained->accepted() && normal_cache_view->accepted()
            && phase_b_cache_view->accepted()
            && retained.get() == normal_cache_view.get()
            && retained.get() == phase_b_cache_view.get();
    }

private:
    explicit RuntimeMetalArchivePipelineReceipt(
        RuntimeMetalArchivePipelineAudit audit) noexcept
        : audit_(std::move(audit)) {}

    RuntimeMetalArchivePipelineAudit audit_{};
    mutable bool metrics_claimed_ = false;
    friend class Runtime;
    friend class RuntimePipelineOperationProof;
    friend class runtime_detail::RuntimeGpuCacheMetricsClaimPlan;
};

static_assert(!std::is_copy_constructible_v<RuntimeMetalArchivePipelineReceipt>);
static_assert(std::is_nothrow_move_constructible_v<
              RuntimeMetalArchivePipelineReceipt>);

inline constexpr std::size_t k_runtime_pipeline_kind_slots = 64u;
inline constexpr std::size_t k_runtime_pipeline_prepared_slots = 11u;

/// Exact 8-bit radix pass count for the persisted Shape-V5 target coordinate
/// domain.  Kept public and allocation-free so portable contract tests can pin
/// every byte-width boundary used by causal dispatch validation.
[[nodiscard]] inline std::optional<std::uint32_t>
runtime_shape_v5_radix_pass_count(
    std::uint64_t target_position_offset,
    std::uint64_t target_len) noexcept {
    if (target_len == 0u
        || target_position_offset
            > (std::numeric_limits<std::uint64_t>::max)()
                - (target_len - 1u)) {
        return std::nullopt;
    }
    const std::uint64_t target_end =
        target_position_offset + target_len - 1u;
    if (target_end >= (std::uint64_t{1} << 63u)) {
        return std::nullopt;
    }
    const std::uint32_t bits = target_end == 0u
        ? 1u
        : 64u - static_cast<std::uint32_t>(
            std::countl_zero(target_end));
    return std::clamp<std::uint32_t>((bits + 7u) / 8u, 1u, 8u);
}

enum class RuntimePipelineOperationKind : std::uint8_t {
    invalid,
    query_sketch,
    shape_v5_coordinate,
};

struct RuntimeGpuCacheMetricsAudit {
    std::uint64_t token_identity = 0u;
    std::uint64_t gpu_pipeline_cache_loads = 0u;
    std::uint64_t gpu_pipeline_cache_seed_bytes = 0u;
    std::uint64_t gpu_pipeline_cache_load_ns = 0u;
    std::uint64_t gpu_pipeline_creates = 0u;
    std::uint64_t gpu_pipeline_creation_ns = 0u;
    bool saturated = false;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        const bool seed_shape = gpu_pipeline_cache_loads == 0u
            ? gpu_pipeline_cache_seed_bytes == 0u
                && gpu_pipeline_cache_load_ns == 0u
            : gpu_pipeline_cache_loads == 1u
                && gpu_pipeline_cache_seed_bytes != 0u
                && gpu_pipeline_cache_load_ns != 0u;
        const bool create_shape = gpu_pipeline_creates == 0u
            ? gpu_pipeline_creation_ns == 0u
            : gpu_pipeline_creation_ns != 0u;
        return token_identity != 0u && !saturated
            && seed_shape && create_shape;
    }
    [[nodiscard]] constexpr bool operator==(
        const RuntimeGpuCacheMetricsAudit&) const noexcept = default;
    [[nodiscard]] constexpr bool componentwise_bounded_by(
        const RuntimeGpuCacheMetricsAudit& maximum) const noexcept {
        return accepted() && maximum.accepted()
            && token_identity == maximum.token_identity
            && gpu_pipeline_cache_loads
                <= maximum.gpu_pipeline_cache_loads
            && gpu_pipeline_cache_seed_bytes
                <= maximum.gpu_pipeline_cache_seed_bytes
            && gpu_pipeline_cache_load_ns
                <= maximum.gpu_pipeline_cache_load_ns
            && gpu_pipeline_creates <= maximum.gpu_pipeline_creates
            && gpu_pipeline_creation_ns
                <= maximum.gpu_pipeline_creation_ns;
    }
};

namespace runtime_detail {

/// Fixed, nonallocating transaction prepared before the final immutable live
/// gate. Construction only observes report-once state. Destruction without
/// commit is a no-op, so a gate failure cannot consume seed/pipeline metrics.
/// Runtime holds its queue/cache mutex across prepare, gate and commit.
class RuntimeGpuCacheMetricsClaimPlan {
public:
    RuntimeGpuCacheMetricsClaimPlan() = default;
    RuntimeGpuCacheMetricsClaimPlan(
        const RuntimeGpuCacheMetricsClaimPlan&) = delete;
    RuntimeGpuCacheMetricsClaimPlan& operator=(
        const RuntimeGpuCacheMetricsClaimPlan&) = delete;
    RuntimeGpuCacheMetricsClaimPlan(
        RuntimeGpuCacheMetricsClaimPlan&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          claims_(std::exchange(other.claims_, {})),
          claim_count_(std::exchange(other.claim_count_, 0u)),
          claim_seed_(std::exchange(other.claim_seed_, false)),
          valid_(std::exchange(other.valid_, false)),
          committed_(std::exchange(other.committed_, false)) {}
    RuntimeGpuCacheMetricsClaimPlan& operator=(
        RuntimeGpuCacheMetricsClaimPlan&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            claims_ = std::exchange(other.claims_, {});
            claim_count_ = std::exchange(other.claim_count_, 0u);
            claim_seed_ = std::exchange(other.claim_seed_, false);
            valid_ = std::exchange(other.valid_, false);
            committed_ = std::exchange(other.committed_, false);
        }
        return *this;
    }

    [[nodiscard]] static RuntimeGpuCacheMetricsClaimPlan prepare(
        std::uint64_t token_identity,
        const RuntimeGpuCacheMetricsAudit& maximum,
        bool seed_already_claimed,
        std::span<const RuntimeMetalArchivePipelineReceipt* const>
            receipts) noexcept {
        RuntimeGpuCacheMetricsClaimPlan plan{};
        plan.audit_.token_identity = token_identity;
        if (token_identity == 0u || !maximum.accepted()
            || maximum.token_identity != token_identity
            || receipts.empty() || receipts.size() > plan.claims_.size()) {
            return plan;
        }
        const RuntimeMetalArchivePipelineReceipt* first = receipts.front();
        if (first == nullptr || !first->accepted()) return plan;
        plan.claim_seed_ = !seed_already_claimed;
        if (plan.claim_seed_) {
            plan.audit_.gpu_pipeline_cache_loads = 1u;
            plan.audit_.gpu_pipeline_cache_seed_bytes =
                first->audit().seed_size;
            plan.audit_.gpu_pipeline_cache_load_ns =
                first->audit().seed_load_ns;
        }
        for (std::size_t i = 0u; i < receipts.size(); ++i) {
            const RuntimeMetalArchivePipelineReceipt* receipt = receipts[i];
            if (receipt == nullptr || !receipt->accepted()
                || receipt->audit().seed_size != first->audit().seed_size
                || receipt->audit().seed_load_ns
                    != first->audit().seed_load_ns) {
                return RuntimeGpuCacheMetricsClaimPlan{};
            }
            for (std::size_t prior = 0u; prior < i; ++prior) {
                if (receipts[prior] == receipt) {
                    return RuntimeGpuCacheMetricsClaimPlan{};
                }
            }
            if (receipt->metrics_claimed_) continue;
            if (plan.claim_count_ == plan.claims_.size()
                || plan.audit_.gpu_pipeline_creates
                    == (std::numeric_limits<std::uint64_t>::max)()
                || receipt->audit().pipeline_create_ns
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - plan.audit_.gpu_pipeline_creation_ns) {
                return RuntimeGpuCacheMetricsClaimPlan{};
            }
            plan.claims_[plan.claim_count_++] = receipt;
            ++plan.audit_.gpu_pipeline_creates;
            plan.audit_.gpu_pipeline_creation_ns +=
                receipt->audit().pipeline_create_ns;
        }
        plan.valid_ = plan.audit_.componentwise_bounded_by(maximum);
        return plan;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return valid_ && audit_.accepted();
    }
    [[nodiscard]] bool committed() const noexcept { return committed_; }
    [[nodiscard]] const RuntimeGpuCacheMetricsAudit& audit()
        const noexcept { return audit_; }

    void commit(bool& seed_claimed) noexcept {
        if (!accepted() || committed_) return;
        if (claim_seed_) seed_claimed = true;
        for (std::size_t i = 0u; i < claim_count_; ++i) {
            claims_[i]->metrics_claimed_ = true;
        }
        committed_ = true;
    }

private:
    RuntimeGpuCacheMetricsAudit audit_{};
    std::array<const RuntimeMetalArchivePipelineReceipt*,
               k_runtime_pipeline_prepared_slots> claims_{};
    std::size_t claim_count_ = 0u;
    bool claim_seed_ = false;
    bool valid_ = false;
    bool committed_ = false;
};

static_assert(!std::is_copy_constructible_v<
    RuntimeGpuCacheMetricsClaimPlan>);
static_assert(std::is_nothrow_move_constructible_v<
    RuntimeGpuCacheMetricsClaimPlan>);

}  // namespace runtime_detail

class RuntimeGpuCacheMetricsDelta {
public:
    RuntimeGpuCacheMetricsDelta() = default;
    RuntimeGpuCacheMetricsDelta(const RuntimeGpuCacheMetricsDelta&) = delete;
    RuntimeGpuCacheMetricsDelta& operator=(
        const RuntimeGpuCacheMetricsDelta&) = delete;
    RuntimeGpuCacheMetricsDelta(
        RuntimeGpuCacheMetricsDelta&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          issued_(std::exchange(other.issued_, false)) {}
    RuntimeGpuCacheMetricsDelta& operator=(
        RuntimeGpuCacheMetricsDelta&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            issued_ = std::exchange(other.issued_, false);
        }
        return *this;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return issued_ && audit_.accepted();
    }
    [[nodiscard]] const RuntimeGpuCacheMetricsAudit& audit() const noexcept {
        return audit_;
    }
    void consume() noexcept {
        DEBUG_ASSERT(accepted(),
            "persisted GPU cache metrics token was not prevalidated");
        audit_ = {};
        issued_ = false;
    }

private:
    RuntimeGpuCacheMetricsAudit audit_{};
    bool issued_ = false;
    friend class Runtime;
};

static_assert(!std::is_copy_constructible_v<RuntimeGpuCacheMetricsDelta>);
static_assert(std::is_nothrow_move_constructible_v<
    RuntimeGpuCacheMetricsDelta>);

enum class RuntimePipelineCachePolicyEvidence : std::uint8_t {
    invalid,
    immutable_preseed_all_hit,
};

struct RuntimeGpuExecutionEnvironmentBinding {
    bool gpu_environment_present = false;
    std::array<std::uint8_t, VK_UUID_SIZE> gpu_device_uuid{};
    std::array<char, 128> gpu_driver_identity{};
    RuntimePipelineCachePolicyEvidence cache_policy =
        RuntimePipelineCachePolicyEvidence::invalid;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    bool pipeline_cache_all_hit = false;

    [[nodiscard]] bool accepted() const noexcept {
        return gpu_environment_present
            && std::any_of(
                gpu_device_uuid.begin(), gpu_device_uuid.end(),
                [](std::uint8_t value) { return value != 0u; })
            && std::any_of(
                gpu_driver_identity.begin(), gpu_driver_identity.end(),
                [](char value) { return value != '\0'; })
            && cache_policy
                == RuntimePipelineCachePolicyEvidence::
                    immutable_preseed_all_hit
            && std::any_of(
                seed_sha256.begin(), seed_sha256.end(),
                [](std::uint8_t value) { return value != 0u; })
            && seed_size != 0u && manifest_checksum != 0u
            && pipeline_cache_all_hit;
    }
    [[nodiscard]] constexpr bool operator==(
        const RuntimeGpuExecutionEnvironmentBinding&) const noexcept = default;
};

struct RuntimePipelinePreparedAudit {
    ShaderVariantKey shader_key{};
    std::uint64_t receipt_identity = 0u;
    std::uint64_t prepared_calls = 0u;
};

struct RuntimePipelineJobUseAudit {
    std::array<std::uint64_t, k_runtime_pipeline_kind_slots> dispatches{};
    std::array<std::uint64_t, k_runtime_pipeline_kind_slots> workgroups{};
    std::uint64_t submission_operation_identity = 0u;
    std::array<std::uint64_t, 4> submission_ordinals{};
    std::uint64_t committed_submissions = 0u;
    std::uint64_t minimizer_count = 0u;
    std::uint64_t target_len = 0u;
    std::uint64_t target_position_offset = 0u;
    std::uint64_t anchor_count = 0u;
    std::uint64_t range_count = 0u;
    bool shape_contract_set = false;
    bool saturated = false;

    [[nodiscard]] std::uint64_t count(ShaderKind kind) const noexcept {
        const std::size_t slot = static_cast<std::size_t>(kind);
        return slot < dispatches.size() ? dispatches[slot] : 0u;
    }
};

struct RuntimePipelineOperationAudit {
    RuntimePipelineOperationKind kind = RuntimePipelineOperationKind::invalid;
    std::uint64_t operation_identity = 0u;
    std::array<RuntimePipelinePreparedAudit,
               k_runtime_pipeline_prepared_slots> prepared{};
    std::size_t prepared_count = 0u;
    std::vector<RuntimePipelineJobUseAudit> jobs{};
    std::uint64_t committed_submissions = 0u;
    std::uint64_t begun_command_generations = 0u;
    std::uint64_t committed_command_generations = 0u;
    std::uint64_t burned_pending_uses = 0u;
    bool pending = false;
    bool invalid = false;
    bool saturated = false;
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    std::uint64_t seed_receipt_identity = 0u;
    RuntimeMetalArchiveSetIdentity archive_set{};
    RuntimeMetalArchiveProcessQualification cache_qualification{};
    RuntimeGpuCacheMetricsAudit cache_metrics{};
};

class RuntimePipelineUseCollector;
class PersistedShapeV5CoordinateOperationProof;

/// Move-only causal proof that binds PREPARED cache receipts to dispatches
/// committed only after submit, fence wait, immutable live gate, and reset.
class RuntimePipelineOperationProof {
public:
    RuntimePipelineOperationProof() = default;
    RuntimePipelineOperationProof(const RuntimePipelineOperationProof&) =
        delete;
    RuntimePipelineOperationProof& operator=(
        const RuntimePipelineOperationProof&) = delete;
    RuntimePipelineOperationProof(
        RuntimePipelineOperationProof&& other) noexcept
        : capacity_permit_(std::move(other.capacity_permit_)),
          audit_(std::move(other.audit_)),
          receipts_(std::exchange(other.receipts_, {})),
          issued_(std::exchange(other.issued_, false)) {
        other.audit_ = {};
    }
    RuntimePipelineOperationProof& operator=(
        RuntimePipelineOperationProof&& other) noexcept {
        if (this != &other) {
            // Vector move-assignment releases the old admitted allocation
            // while its permit is still live.  Only then may the old permit
            // be replaced by the source permit.
            audit_ = std::move(other.audit_);
            capacity_permit_ = std::move(other.capacity_permit_);
            receipts_ = std::exchange(other.receipts_, {});
            issued_ = std::exchange(other.issued_, false);
            other.audit_ = {};
        }
        return *this;
    }

    [[nodiscard]] const RuntimePipelineOperationAudit& audit()
        const noexcept { return audit_; }
    [[nodiscard]] bool accepted_query_sketch() const noexcept;
    [[nodiscard]] bool accepted_shape_v5_coordinate() const noexcept;
    [[nodiscard]] static bool self_test_causal_collector_contract(
        RuntimeMetalArchivePipelineAudit base_audit);

private:
    [[nodiscard]] bool accepted_common() const noexcept;
    [[nodiscard]] std::uint64_t prepared_calls(
        ShaderKind kind) const noexcept;
    [[nodiscard]] bool prepared_exactly(
        std::span<const ShaderKind> required,
        std::uint64_t radix_prepared_calls = 0u) const noexcept;

    // Declared before the vector owner so destruction deallocates jobs before
    // releasing their reservation (members are destroyed in reverse order).
    bioseq::parallelism::MemoryPermit capacity_permit_{};
    RuntimePipelineOperationAudit audit_{};
    std::array<const RuntimeMetalArchivePipelineReceipt*,
               k_runtime_pipeline_prepared_slots> receipts_{};
    bool issued_ = false;
    friend class Runtime;
    friend class RuntimePipelineUseCollector;
    friend class PersistedShapeV5CoordinateOperationProof;
};

static_assert(!std::is_copy_constructible_v<RuntimePipelineOperationProof>);
static_assert(std::is_nothrow_move_constructible_v<
    RuntimePipelineOperationProof>);

inline bool RuntimePipelineOperationProof::accepted_common() const noexcept {
    if (!issued_ || audit_.operation_identity == 0u
        || audit_.kind == RuntimePipelineOperationKind::invalid
        || audit_.jobs.empty() || audit_.prepared_count == 0u
        || audit_.prepared_count > audit_.prepared.size()
        || audit_.committed_submissions == 0u
        || audit_.committed_command_generations
            != audit_.committed_submissions
        || audit_.begun_command_generations
            != audit_.committed_command_generations
        || audit_.burned_pending_uses != 0u || audit_.pending
        || audit_.invalid || audit_.saturated
        || !audit_.cache_metrics.accepted()
        || audit_.cache_metrics.token_identity != audit_.operation_identity
        || audit_.api_version == 0u || audit_.driver_version == 0u
        || audit_.seed_size == 0u || audit_.manifest_checksum == 0u
        || audit_.seed_receipt_identity == 0u
        || !audit_.archive_set.accepted()
        || !audit_.cache_qualification.accepted()
        || audit_.cache_qualification.archive_set != audit_.archive_set
        || std::all_of(
            audit_.device_uuid.begin(), audit_.device_uuid.end(),
            [](std::uint8_t value) { return value == 0u; })
        || std::all_of(
            audit_.pipeline_cache_uuid.begin(),
            audit_.pipeline_cache_uuid.end(),
            [](std::uint8_t value) { return value == 0u; })
        || std::all_of(
            audit_.seed_sha256.begin(), audit_.seed_sha256.end(),
            [](std::uint8_t value) { return value == 0u; })) {
        return false;
    }
    for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
        const RuntimePipelinePreparedAudit& prepared = audit_.prepared[i];
        const RuntimeMetalArchivePipelineReceipt* receipt = receipts_[i];
        if (receipt == nullptr || !receipt->accepted()
            || prepared.prepared_calls == 0u
            || prepared.receipt_identity == 0u
            || !valid_shader_variant_key(prepared.shader_key)) {
            return false;
        }
        const RuntimeMetalArchivePipelineAudit& pipeline = receipt->audit();
        if (pipeline.receipt_identity != prepared.receipt_identity
            || pipeline.shader_key != prepared.shader_key
            || pipeline.runtime_vulkan_device_uuid != audit_.device_uuid
            || pipeline.api_version != audit_.api_version
            || pipeline.driver_version != audit_.driver_version
            || pipeline.vendor_id != audit_.vendor_id
            || pipeline.device_id != audit_.device_id
            || pipeline.pipeline_cache_uuid != audit_.pipeline_cache_uuid
            || pipeline.seed_sha256 != audit_.seed_sha256
            || pipeline.seed_size != audit_.seed_size
            || pipeline.manifest_checksum != audit_.manifest_checksum
            || pipeline.seed_receipt_identity
                != audit_.seed_receipt_identity
            || pipeline.expected_archives != audit_.archive_set
            || pipeline.cache_qualification
                != audit_.cache_qualification) {
            return false;
        }
    }
    for (const RuntimePipelineJobUseAudit& job : audit_.jobs) {
        if (job.saturated || job.committed_submissions == 0u) return false;
        if (job.submission_operation_identity != audit_.operation_identity
            || job.committed_submissions > job.submission_ordinals.size()) {
            return false;
        }
        for (std::size_t i = 0u;
             i < job.submission_ordinals.size(); ++i) {
            if (i < job.committed_submissions) {
                if (job.submission_ordinals[i] == 0u) return false;
                for (std::size_t prior = 0u; prior < i; ++prior) {
                    if (job.submission_ordinals[prior]
                        == job.submission_ordinals[i]) return false;
                }
            } else if (job.submission_ordinals[i] != 0u) {
                return false;
            }
        }
        for (std::size_t kind = 0u;
             kind < job.dispatches.size(); ++kind) {
            if ((job.dispatches[kind] == 0u)
                    != (job.workgroups[kind] == 0u)
                || (job.dispatches[kind] != 0u
                    && job.workgroups[kind] < job.dispatches[kind])) {
                return false;
            }
        }
    }
    return true;
}

inline std::uint64_t RuntimePipelineOperationProof::prepared_calls(
    ShaderKind kind) const noexcept {
    const std::uint32_t raw = static_cast<std::uint32_t>(kind);
    for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
        if (audit_.prepared[i].shader_key.kind == raw) {
            return audit_.prepared[i].prepared_calls;
        }
    }
    return 0u;
}

inline bool RuntimePipelineOperationProof::prepared_exactly(
    std::span<const ShaderKind> required,
    std::uint64_t radix_prepared_calls) const noexcept {
    const std::size_t expected_count = required.size()
        + (radix_prepared_calls == 0u ? 0u : 3u);
    if (audit_.prepared_count != expected_count) return false;
    for (const ShaderKind kind : required) {
        if (prepared_calls(kind) != 1u) return false;
    }
    const std::array radix{
        ShaderKind::phase_c_sort_radix_histogram,
        ShaderKind::phase_c_sort_radix_scan,
        ShaderKind::phase_c_sort_radix_scatter,
    };
    for (const ShaderKind kind : radix) {
        if (prepared_calls(kind) != radix_prepared_calls) return false;
    }
    for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
        const ShaderKind actual = static_cast<ShaderKind>(
            audit_.prepared[i].shader_key.kind);
        const bool base = std::find(required.begin(), required.end(), actual)
            != required.end();
        const bool sort = std::find(radix.begin(), radix.end(), actual)
            != radix.end();
        if (!base && !(radix_prepared_calls != 0u && sort)) return false;
    }
    return true;
}

inline bool RuntimePipelineOperationProof::accepted_query_sketch()
    const noexcept {
    constexpr std::array required{
        ShaderKind::phase_c_sketch_tile,
        ShaderKind::phase_c_prefix_scan_block,
        ShaderKind::phase_c_prefix_scan_add,
    };
    if (!accepted_common()
        || audit_.kind != RuntimePipelineOperationKind::query_sketch
        || audit_.jobs.size() != 1u || audit_.committed_submissions != 2u
        || !prepared_exactly(required)) {
        return false;
    }
    const RuntimePipelineJobUseAudit& job = audit_.jobs.front();
    if (job.committed_submissions != 2u
        || job.count(ShaderKind::phase_c_sketch_tile) != 2u
        || job.count(ShaderKind::phase_c_prefix_scan_block) != 1u
        || job.count(ShaderKind::phase_c_prefix_scan_add) != 2u) {
        return false;
    }
    std::uint64_t total = 0u;
    for (const std::uint64_t count : job.dispatches) {
        if (count > (std::numeric_limits<std::uint64_t>::max)() - total) {
            return false;
        }
        total += count;
    }
    return total == 5u;
}

inline bool RuntimePipelineOperationProof::accepted_shape_v5_coordinate()
    const noexcept {
    constexpr std::array required{
        ShaderKind::phase_c_shape_v5_seed_count,
        ShaderKind::phase_c_shape_v5_seed_emit,
        ShaderKind::phase_c_prefix_scan_block,
        ShaderKind::phase_c_prefix_scan_add,
        ShaderKind::phase_c_chain_range_build,
        ShaderKind::phase_c_coordinate_reduce,
        ShaderKind::chain_tiled_dp_u64,
    };
    if (!accepted_common()
        || audit_.kind
            != RuntimePipelineOperationKind::shape_v5_coordinate) {
        return false;
    }
    std::uint64_t anchored_jobs = 0u;
    for (const RuntimePipelineJobUseAudit& job : audit_.jobs) {
        if (!job.shape_contract_set || job.minimizer_count == 0u
            || job.target_len == 0u
            || job.target_position_offset
                > (std::numeric_limits<std::uint64_t>::max)()
                    - (job.target_len - 1u)) {
            return false;
        }
        const auto pass_count = runtime_shape_v5_radix_pass_count(
            job.target_position_offset, job.target_len);
        if (!pass_count.has_value()) return false;
        const std::uint64_t passes = *pass_count;
        const bool front =
            job.count(ShaderKind::phase_c_shape_v5_seed_count) == 1u
            && job.count(ShaderKind::phase_c_prefix_scan_block) == 1u
            && job.count(ShaderKind::phase_c_prefix_scan_add) == 2u;
        if (!front) return false;
        std::uint64_t expected_total = 4u;
        std::uint64_t expected_submissions = 1u;
        if (job.anchor_count == 0u) {
            if (job.range_count != 0u) return false;
        } else {
            ++anchored_jobs;
            expected_total = 6u + 3u * passes;
            expected_submissions = 3u;
            if (job.count(ShaderKind::phase_c_shape_v5_seed_emit) != 1u
                || job.count(ShaderKind::phase_c_sort_radix_histogram)
                    != passes
                || job.count(ShaderKind::phase_c_sort_radix_scan) != passes
                || job.count(ShaderKind::phase_c_sort_radix_scatter)
                    != passes
                || job.count(ShaderKind::phase_c_chain_range_build) != 1u) {
                return false;
            }
            if (job.range_count != 0u) {
                expected_total += 2u;
                expected_submissions = 4u;
                if (job.count(ShaderKind::chain_tiled_dp_u64) != 1u
                    || job.count(ShaderKind::phase_c_coordinate_reduce)
                        != 1u) {
                    return false;
                }
            }
        }
        std::uint64_t actual_total = 0u;
        for (const std::uint64_t count : job.dispatches) {
            if (count > (std::numeric_limits<std::uint64_t>::max)()
                    - actual_total) {
                return false;
            }
            actual_total += count;
        }
        if (actual_total != expected_total
            || job.committed_submissions != expected_submissions
            || job.count(ShaderKind::phase_c_coordinate_vote_reduce) != 0u
            || job.count(
                ShaderKind::phase_c_coordinate_vote_reduce_batch) != 0u
            || job.count(ShaderKind::phase_c_sketch_tile) != 0u) {
            return false;
        }
    }
    return prepared_exactly(required, anchored_jobs);
}

class RuntimePipelineUseCollector {
public:
    RuntimePipelineUseCollector(const RuntimePipelineUseCollector&) = delete;
    RuntimePipelineUseCollector& operator=(
        const RuntimePipelineUseCollector&) = delete;
    RuntimePipelineUseCollector(RuntimePipelineUseCollector&&) = delete;
    RuntimePipelineUseCollector& operator=(
        RuntimePipelineUseCollector&&) = delete;
    ~RuntimePipelineUseCollector() { burn_pending(); }

private:
    struct PendingJob {
        std::array<std::uint64_t,
                   k_runtime_pipeline_kind_slots> dispatches{};
        std::array<std::uint64_t,
                   k_runtime_pipeline_kind_slots> workgroups{};
    };

    [[nodiscard]] static std::unique_ptr<RuntimePipelineUseCollector> create(
        RuntimePipelineOperationKind kind,
        std::size_t job_count,
        std::uint64_t operation_identity) {
        using bioseq::parallelism::saturating_add;
        using bioseq::parallelism::saturating_mul;
        if (kind == RuntimePipelineOperationKind::invalid
            || job_count == 0u || operation_identity == 0u
            || job_count
                > std::vector<RuntimePipelineJobUseAudit>{}.max_size()
            || job_count > std::vector<PendingJob>{}.max_size()) {
            throw VulkanRuntimeError(
                "runtime pipeline receipt collector shape is invalid");
        }
        std::uint64_t retained = sizeof(RuntimePipelineUseCollector);
        retained = saturating_add(retained, 2u * 1024u);
        retained = saturating_add(
            retained,
            saturating_mul(
                static_cast<std::uint64_t>(job_count),
                sizeof(RuntimePipelineJobUseAudit)));
        retained = saturating_add(
            retained,
            saturating_mul(
                static_cast<std::uint64_t>(job_count),
                sizeof(PendingJob)));
        const std::uint64_t peak = saturating_mul(
            retained,
            bioseq::parallelism::kVectorPreallocationHeadroomFactor);
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw VulkanRuntimeError(
                "runtime pipeline receipt collector admission saturated");
        }
        bioseq::parallelism::MemoryAdmissionFailure failure{};
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(peak, &failure);
        if (!permit) {
            throw VulkanRuntimeError(shader_cache_message(
                "runtime pipeline receipt collector admission denied: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(
                    failure.reason),
                " requested=", failure.requested_bytes));
        }
        std::unique_ptr<RuntimePipelineUseCollector> result;
        try {
            result.reset(new RuntimePipelineUseCollector(
                kind, job_count, operation_identity, std::move(*permit)));
        } catch (const std::bad_alloc&) {
            throw VulkanRuntimeError(
                "runtime pipeline receipt collector allocation failed after admission");
        }
        result->reconcile_capacity_permit();
        return result;
    }

    RuntimePipelineUseCollector(
        RuntimePipelineOperationKind kind,
        std::size_t job_count,
        std::uint64_t operation_identity,
        bioseq::parallelism::MemoryPermit capacity_permit)
        : capacity_permit_(std::move(capacity_permit)),
          pending_(job_count) {
        audit_.kind = kind;
        audit_.operation_identity = operation_identity;
        audit_.jobs.resize(job_count);
        audit_.cache_metrics.token_identity = operation_identity;
        submission_ordinal_cursor_ = 0u;
    }

    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        using bioseq::parallelism::saturating_add;
        using bioseq::parallelism::saturating_mul;
        std::uint64_t retained = sizeof(RuntimePipelineUseCollector);
        retained = saturating_add(retained, 2u * 1024u);
        retained = saturating_add(
            retained,
            saturating_mul(
                static_cast<std::uint64_t>(audit_.jobs.capacity()),
                sizeof(RuntimePipelineJobUseAudit)));
        retained = saturating_add(
            retained,
            saturating_mul(
                static_cast<std::uint64_t>(pending_.capacity()),
                sizeof(PendingJob)));
        return retained;
    }

    void reconcile_capacity_permit() {
        const std::uint64_t retained = retained_capacity_bytes();
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || !capacity_permit_
            || retained > capacity_permit_.bytes()) {
            throw VulkanRuntimeError(
                "runtime pipeline receipt collector exceeded admitted capacity");
        }
        capacity_permit_.shrink_to(retained);
    }

    static bool add_counter(
        std::uint64_t& destination, std::uint64_t value) noexcept {
        if (value > (std::numeric_limits<std::uint64_t>::max)()
                - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            return false;
        }
        destination += value;
        return true;
    }

    void invalidate() noexcept { audit_.invalid = true; }

    void prepare(
        const ShaderVariantKey& key,
        const RuntimeMetalArchivePipelineReceipt* receipt) noexcept {
        if (audit_.invalid || receipt == nullptr || !receipt->accepted()
            || receipt->audit().shader_key != key
            || receipt->audit().pipeline_create_ns == 0u) {
            invalidate();
            return;
        }
        std::size_t slot = audit_.prepared_count;
        for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
            if (audit_.prepared[i].shader_key == key) {
                slot = i;
                break;
            }
        }
        if (slot == audit_.prepared_count) {
            if (slot == audit_.prepared.size()) {
                invalidate();
                return;
            }
            audit_.prepared[slot] = RuntimePipelinePreparedAudit{
                .shader_key = key,
                .receipt_identity = receipt->audit().receipt_identity,
                .prepared_calls = 0u,
            };
            receipts_[slot] = receipt;
            ++audit_.prepared_count;
        } else if (receipts_[slot] != receipt
                   || audit_.prepared[slot].receipt_identity
                       != receipt->audit().receipt_identity) {
            invalidate();
            return;
        }
        if (!add_counter(audit_.prepared[slot].prepared_calls, 1u)) {
            audit_.saturated = true;
            return;
        }

        const RuntimeMetalArchivePipelineAudit& source = receipt->audit();
        if (!environment_sealed_) {
            audit_.device_uuid = source.runtime_vulkan_device_uuid;
            audit_.api_version = source.api_version;
            audit_.driver_version = source.driver_version;
            audit_.vendor_id = source.vendor_id;
            audit_.device_id = source.device_id;
            audit_.pipeline_cache_uuid = source.pipeline_cache_uuid;
            audit_.seed_sha256 = source.seed_sha256;
            audit_.seed_size = source.seed_size;
            audit_.manifest_checksum = source.manifest_checksum;
            audit_.seed_receipt_identity = source.seed_receipt_identity;
            audit_.archive_set = source.expected_archives;
            audit_.cache_qualification = source.cache_qualification;
            environment_sealed_ = true;
        } else if (source.runtime_vulkan_device_uuid != audit_.device_uuid
                   || source.api_version != audit_.api_version
                   || source.driver_version != audit_.driver_version
                   || source.vendor_id != audit_.vendor_id
                   || source.device_id != audit_.device_id
                   || source.pipeline_cache_uuid
                       != audit_.pipeline_cache_uuid
                   || source.seed_sha256 != audit_.seed_sha256
                   || source.seed_size != audit_.seed_size
                   || source.manifest_checksum != audit_.manifest_checksum
                   || source.seed_receipt_identity
                       != audit_.seed_receipt_identity
                   || source.expected_archives != audit_.archive_set
                   || source.cache_qualification
                       != audit_.cache_qualification) {
            invalidate();
            return;
        }
        // Report-once counters are deliberately not claimed during PREPARE.
        // A command that later resets, burns, or fails causal validation must
        // not steal metrics from the next wholly accepted operation. The
        // finalizer claims the canonical seed/receipt bits transactionally
        // under Runtime::mu_ after the immutable live gate succeeds.
    }

    void set_shape_input(
        std::size_t job_index,
        std::uint64_t minimizer_count,
        std::uint64_t target_len,
        std::uint64_t target_position_offset) noexcept {
        if (job_index >= audit_.jobs.size()) {
            invalidate();
            return;
        }
        RuntimePipelineJobUseAudit& job = audit_.jobs[job_index];
        job.minimizer_count = minimizer_count;
        job.target_len = target_len;
        job.target_position_offset = target_position_offset;
        job.shape_contract_set = true;
    }

    void set_shape_outcome(
        std::size_t job_index,
        std::uint64_t anchor_count,
        std::uint64_t range_count) noexcept {
        if (job_index >= audit_.jobs.size()
            || !audit_.jobs[job_index].shape_contract_set) {
            invalidate();
            return;
        }
        audit_.jobs[job_index].anchor_count = anchor_count;
        audit_.jobs[job_index].range_count = range_count;
    }

    void begin(VkCommandBuffer command_buffer) noexcept {
        if (active_) {
            burn_pending();
            invalidate();
        }
        if (command_buffer == VK_NULL_HANDLE || audit_.invalid) {
            invalidate();
            return;
        }
        active_ = true;
        command_identity_ = vulkan_handle_identity(command_buffer);
        if (command_identity_ == 0u
            || !add_counter(audit_.begun_command_generations, 1u)) {
            audit_.saturated = true;
            invalidate();
            return;
        }
        active_generation_ = audit_.begun_command_generations;
    }

    void use(
        VkCommandBuffer command_buffer,
        std::size_t job_index,
        const ShaderVariantKey& key,
        const RuntimeMetalArchivePipelineReceipt* receipt,
        std::uint32_t group_count,
        std::uint64_t observed_generation = 0u) noexcept {
        if (!active_ || audit_.invalid || job_index >= pending_.size()
            || vulkan_handle_identity(command_buffer) != command_identity_
            || group_count == 0u
            || (observed_generation != 0u
                && observed_generation != active_generation_)
            || receipt == nullptr || !receipt->accepted()) {
            invalidate();
            return;
        }
        bool prepared = false;
        for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
            if (audit_.prepared[i].shader_key == key
                && receipts_[i] == receipt
                && audit_.prepared[i].receipt_identity
                    == receipt->audit().receipt_identity) {
                prepared = true;
                break;
            }
        }
        const std::size_t kind = key.kind;
        if (!prepared || kind >= k_runtime_pipeline_kind_slots
            || !add_counter(pending_[job_index].dispatches[kind], 1u)
            || !add_counter(
                pending_[job_index].workgroups[kind], group_count)) {
            audit_.jobs[job_index].saturated = true;
            audit_.saturated = true;
            invalidate();
            return;
        }
        audit_.pending = true;
    }

public:
    void commit(VkCommandBuffer command_buffer) noexcept {
        if (!active_
            || vulkan_handle_identity(command_buffer) != command_identity_) {
            burn_pending();
            invalidate();
            return;
        }
        bool any = false;
        if (submission_ordinal_cursor_
            == (std::numeric_limits<std::uint64_t>::max)()) {
            burn_pending();
            audit_.saturated = true;
            invalidate();
            return;
        }
        const std::uint64_t submission_ordinal =
            ++submission_ordinal_cursor_;
        for (std::size_t job_index = 0u;
             job_index < pending_.size(); ++job_index) {
            bool touched = false;
            for (std::size_t kind = 0u;
                 kind < k_runtime_pipeline_kind_slots; ++kind) {
                const std::uint64_t count =
                    pending_[job_index].dispatches[kind];
                if (count == 0u) continue;
                touched = true;
                any = true;
                if (!add_counter(
                        audit_.jobs[job_index].dispatches[kind], count)) {
                    audit_.jobs[job_index].saturated = true;
                    audit_.saturated = true;
                }
                if (!add_counter(
                        audit_.jobs[job_index].workgroups[kind],
                        pending_[job_index].workgroups[kind])) {
                    audit_.jobs[job_index].saturated = true;
                    audit_.saturated = true;
                }
                pending_[job_index].dispatches[kind] = 0u;
                pending_[job_index].workgroups[kind] = 0u;
            }
            if (touched) {
                RuntimePipelineJobUseAudit& job = audit_.jobs[job_index];
                if (job.committed_submissions
                        >= job.submission_ordinals.size()
                    || submission_ordinal == 0u
                    || (job.submission_operation_identity != 0u
                        && job.submission_operation_identity
                            != audit_.operation_identity)) {
                    job.saturated = true;
                    audit_.saturated = true;
                    invalidate();
                } else {
                    job.submission_operation_identity =
                        audit_.operation_identity;
                    job.submission_ordinals[
                        job.committed_submissions] = submission_ordinal;
                    if (!add_counter(job.committed_submissions, 1u)) {
                        job.saturated = true;
                        audit_.saturated = true;
                    }
                }
            }
        }
        if (!any || !add_counter(audit_.committed_submissions, 1u)
            || !add_counter(audit_.committed_command_generations, 1u)) {
            invalidate();
        }
        active_ = false;
        command_identity_ = 0u;
        active_generation_ = 0u;
        audit_.pending = false;
    }

    void burn_pending() noexcept {
        if (!active_) return;
        for (auto& job : pending_) {
            for (std::size_t kind = 0u;
                 kind < job.dispatches.size(); ++kind) {
                std::uint64_t& count = job.dispatches[kind];
                if (count != 0u
                    && !add_counter(audit_.burned_pending_uses, count)) {
                    audit_.saturated = true;
                }
                count = 0u;
                job.workgroups[kind] = 0u;
            }
        }
        active_ = false;
        command_identity_ = 0u;
        active_generation_ = 0u;
        audit_.pending = false;
    }

    void burn_before_reset(VkCommandBuffer command_buffer) noexcept {
        if (!active_) return;
        if (vulkan_handle_identity(command_buffer) != command_identity_) {
            invalidate();
        }
        burn_pending();
        // A begun generation that reaches reset without a causal commit is
        // never an accepted empty command.  This also rejects the no-use case
        // where burned_pending_uses alone would remain zero.
        invalidate();
    }

private:
    [[nodiscard]] RuntimePipelineOperationProof seal() noexcept {
        if (active_) {
            audit_.pending = true;
            burn_pending();
            invalidate();
        }
        RuntimePipelineOperationProof proof{};
        proof.audit_ = std::move(audit_);
        proof.receipts_ = receipts_;
        proof.issued_ = true;
        // Pending storage no longer contributes to the sealed proof. Destroy
        // it before shrinking/transferring the permit. Keep the collector
        // object itself covered until its unique_ptr owner destroys it; the
        // resulting small over-reservation then travels with the jobs vector
        // for the complete lifetime of the move-only proof.
        {
            std::vector<PendingJob> empty;
            pending_.swap(empty);
        }
        {
            // The standard allocator transfers this capacity, but explicitly
            // clear any valid moved-from residue before the permit leaves the
            // collector so the ownership proof is allocator-independent.
            std::vector<RuntimePipelineJobUseAudit> empty;
            audit_.jobs.swap(empty);
        }
        const std::uint64_t retained_jobs =
            bioseq::parallelism::saturating_add(
                sizeof(RuntimePipelineUseCollector),
                bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(proof.audit_.jobs.capacity()),
                sizeof(RuntimePipelineJobUseAudit)));
        if (retained_jobs
                == (std::numeric_limits<std::uint64_t>::max)()
            || retained_jobs > capacity_permit_.bytes()) {
            proof.audit_.invalid = true;
        } else {
            capacity_permit_.shrink_to(retained_jobs);
        }
        proof.capacity_permit_ = std::move(capacity_permit_);
        receipts_.fill(nullptr);
        audit_ = {};
        return proof;
    }

    // Kept before both vector owners: they release capacity before the permit.
    bioseq::parallelism::MemoryPermit capacity_permit_{};
    RuntimePipelineOperationAudit audit_{};
    std::array<const RuntimeMetalArchivePipelineReceipt*,
               k_runtime_pipeline_prepared_slots> receipts_{};
    std::vector<PendingJob> pending_{};
    std::uint64_t command_identity_ = 0u;
    std::uint64_t active_generation_ = 0u;
    std::uint64_t submission_ordinal_cursor_ = 0u;
    bool environment_sealed_ = false;
    bool active_ = false;

    friend class Runtime;
    friend class RuntimePipelineOperationProof;
};

inline bool RuntimePipelineOperationProof::
self_test_causal_collector_contract(
    RuntimeMetalArchivePipelineAudit base_audit) {
    if (!base_audit.accepted() || base_audit.pipeline_create_ns == 0u) {
        return false;
    }
    const std::array<ShaderVariantKey, 11> keys{
        make_shader_key(
            ShaderKind::phase_c_sketch_tile, 384u, 64u, 10u),
        make_shader_key(
            ShaderKind::phase_c_prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(
            ShaderKind::phase_c_prefix_scan_add, 384u, 128u, 1u),
        make_shader_key(
            ShaderKind::phase_c_shape_v5_seed_count, 384u, 128u, 3u),
        make_shader_key(
            ShaderKind::phase_c_shape_v5_seed_emit, 384u, 128u, 3u),
        make_shader_key(
            ShaderKind::phase_c_chain_range_build, 384u, 1u, 1u),
        make_shader_key(
            ShaderKind::phase_c_coordinate_reduce, 384u, 128u, 1u),
        make_shader_key(
            ShaderKind::chain_tiled_dp_u64, 32u, 32u, 0u),
        make_shader_key(
            ShaderKind::phase_c_sort_radix_histogram, 384u, 1u, 1u),
        make_shader_key(
            ShaderKind::phase_c_sort_radix_scan, 384u, 1u, 1u),
        make_shader_key(
            ShaderKind::phase_c_sort_radix_scatter, 384u, 1u, 1u),
    };
    if (!std::all_of(
            keys.begin(), keys.end(),
            [](const ShaderVariantKey& key) {
                return valid_shader_variant_key(key);
            })) {
        return false;
    }
    std::array<RuntimeMetalArchivePipelineReceipt, 11> receipts{};
    bool seed_claimed = false;
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        RuntimeMetalArchivePipelineAudit audit = base_audit;
        audit.receipt_identity = 1000u + i;
        audit.scope_identity = 2000u + i;
        audit.shader_key = keys[i];
        receipts[i] = RuntimeMetalArchivePipelineReceipt{
            std::move(audit)};
        if (!receipts[i].accepted()) return false;
    }
    const auto command = [](std::uintptr_t identity) {
        return reinterpret_cast<VkCommandBuffer>(identity);
    };
    const auto prepare_query = [&keys, &receipts](
        RuntimePipelineUseCollector& collector) {
        for (std::size_t i = 0u; i < 3u; ++i) {
            collector.prepare(keys[i], &receipts[i]);
        }
    };
    const auto query_golden = [&]() {
        auto collector = RuntimePipelineUseCollector::create(
            RuntimePipelineOperationKind::query_sketch, 1u, 101u);
        prepare_query(*collector);
        collector->begin(command(1u));
        collector->use(command(1u), 0u, keys[0], &receipts[0], 3u);
        collector->use(command(1u), 0u, keys[1], &receipts[1], 2u);
        collector->use(command(1u), 0u, keys[2], &receipts[2], 2u);
        collector->commit(command(1u));
        collector->begin(command(2u));
        collector->use(command(2u), 0u, keys[0], &receipts[0], 3u);
        collector->use(command(2u), 0u, keys[2], &receipts[2], 1u);
        collector->commit(command(2u));
        return collector->seal();
    };

    RuntimePipelineOperationProof query{};
    try {
        query = query_golden();
    } catch (...) {
        return false;
    }
    if (!query.accepted_query_sketch()
        || query.audit_.begun_command_generations != 2u
        || query.audit_.committed_command_generations != 2u
        || query.audit_.jobs[0].submission_operation_identity != 101u
        || query.audit_.jobs[0].submission_ordinals[0] != 1u
        || query.audit_.jobs[0].submission_ordinals[1] != 2u
        || query.audit_.jobs[0].count(
            ShaderKind::phase_c_sketch_tile) != 2u
        || query.audit_.jobs[0].workgroups[static_cast<std::size_t>(
            ShaderKind::phase_c_sketch_tile)] != 6u
        || query.audit_.jobs[0].workgroups[static_cast<std::size_t>(
            ShaderKind::phase_c_prefix_scan_block)] != 2u
        || query.audit_.jobs[0].workgroups[static_cast<std::size_t>(
            ShaderKind::phase_c_prefix_scan_add)] != 3u
        || !query.capacity_permit_) {
        return false;
    }
    RuntimePipelineOperationProof moved_query{std::move(query)};
    if (query.accepted_query_sketch() || query.capacity_permit_
        || !moved_query.accepted_query_sketch()
        || !moved_query.capacity_permit_) {
        return false;
    }
    query = std::move(moved_query);
    if (moved_query.accepted_query_sketch()
        || moved_query.capacity_permit_
        || !query.accepted_query_sketch()) {
        return false;
    }

    const auto rejected_query = [&](std::uint64_t identity, int mode) {
        auto collector = RuntimePipelineUseCollector::create(
            RuntimePipelineOperationKind::query_sketch, 1u, identity);
        prepare_query(*collector);
        const VkCommandBuffer begun =
            command(static_cast<std::uintptr_t>(identity));
        collector->begin(begun);
        if (mode != 1) {
            collector->use(
                mode == 4 ? command(
                    static_cast<std::uintptr_t>(identity + 1000u)) : begun,
                0u, keys[0], mode == 5 ? &receipts[1] : &receipts[0],
                mode == 6 ? 0u : 1u,
                mode == 2 ? 2u : 0u);
        }
        if (mode <= 2 || mode >= 4) {
            collector->burn_before_reset(begun);
        }
        return collector->seal();
    };
    try {
        const RuntimePipelineOperationProof reset = rejected_query(102u, 0);
        const RuntimePipelineOperationProof empty = rejected_query(103u, 1);
        const RuntimePipelineOperationProof stale = rejected_query(104u, 2);
        const RuntimePipelineOperationProof uncommitted =
            rejected_query(105u, 3);
        const RuntimePipelineOperationProof wrong_command =
            rejected_query(106u, 4);
        const RuntimePipelineOperationProof wrong_receipt =
            rejected_query(107u, 5);
        const RuntimePipelineOperationProof zero_groups =
            rejected_query(108u, 6);
        if (reset.accepted_query_sketch()
            || reset.audit_.burned_pending_uses != 1u
            || empty.accepted_query_sketch()
            || empty.audit_.begun_command_generations != 1u
            || stale.accepted_query_sketch()
            || uncommitted.accepted_query_sketch()
            || wrong_command.accepted_query_sketch()
            || wrong_receipt.accepted_query_sketch()
            || zero_groups.accepted_query_sketch()) {
            return false;
        }
    } catch (...) {
        return false;
    }

    RuntimePipelineOperationProof shape{};
    try {
        auto collector = RuntimePipelineUseCollector::create(
            RuntimePipelineOperationKind::shape_v5_coordinate, 3u, 202u);
        constexpr std::array<std::size_t, 7> base_slots{
            3u, 4u, 1u, 2u, 5u, 6u, 7u};
        for (const std::size_t slot : base_slots) {
            collector->prepare(keys[slot], &receipts[slot]);
        }
        // One radix pipeline retrieval per anchored job, independent of the
        // number of byte passes recorded in that job's command.
        for (std::size_t repeat = 0u; repeat < 2u; ++repeat) {
            for (std::size_t slot = 8u; slot < 11u; ++slot) {
                collector->prepare(keys[slot], &receipts[slot]);
            }
        }
        collector->set_shape_input(0u, 1u, 1u, 0u);
        collector->set_shape_input(1u, 1u, 255u, 0u);
        collector->set_shape_input(2u, 1u, 65536u, 0u);
        collector->set_shape_outcome(0u, 0u, 0u);
        collector->set_shape_outcome(1u, 1u, 0u);
        collector->set_shape_outcome(2u, 1u, 1u);

        collector->begin(command(11u));
        for (std::size_t job = 0u; job < 3u; ++job) {
            collector->use(command(11u), job, keys[3], &receipts[3], 1u);
            collector->use(command(11u), job, keys[1], &receipts[1], 1u);
            collector->use(command(11u), job, keys[2], &receipts[2], 1u);
            collector->use(command(11u), job, keys[2], &receipts[2], 1u);
        }
        collector->commit(command(11u));

        collector->begin(command(12u));
        for (std::size_t job = 1u; job < 3u; ++job) {
            collector->use(command(12u), job, keys[4], &receipts[4], 1u);
        }
        collector->commit(command(12u));

        collector->begin(command(13u));
        for (std::size_t slot = 8u; slot < 11u; ++slot) {
            collector->use(command(13u), 1u, keys[slot], &receipts[slot], 1u);
            collector->use(command(13u), 2u, keys[slot], &receipts[slot], 1u);
            collector->use(command(13u), 2u, keys[slot], &receipts[slot], 1u);
        }
        collector->use(command(13u), 1u, keys[5], &receipts[5], 1u);
        collector->use(command(13u), 2u, keys[5], &receipts[5], 1u);
        collector->commit(command(13u));

        collector->begin(command(14u));
        collector->use(command(14u), 2u, keys[7], &receipts[7], 1u);
        collector->use(command(14u), 2u, keys[6], &receipts[6], 1u);
        collector->commit(command(14u));
        shape = collector->seal();
    } catch (...) {
        return false;
    }
    if (!shape.accepted_shape_v5_coordinate()
        || shape.audit_.begun_command_generations != 4u
        || shape.audit_.committed_command_generations != 4u
        || !shape.capacity_permit_) {
        return false;
    }
    constexpr std::array<std::uint64_t, 3> expected_submissions{1u, 3u, 4u};
    for (std::size_t job = 0u; job < shape.audit_.jobs.size(); ++job) {
        const RuntimePipelineJobUseAudit& audit = shape.audit_.jobs[job];
        if (audit.submission_operation_identity != 202u
            || audit.committed_submissions != expected_submissions[job]
            || audit.count(
                ShaderKind::phase_c_coordinate_vote_reduce) != 0u
            || audit.count(
                ShaderKind::phase_c_coordinate_vote_reduce_batch) != 0u) {
            return false;
        }
        for (std::size_t ordinal = 0u;
             ordinal < audit.committed_submissions; ++ordinal) {
            if (audit.submission_ordinals[ordinal] != ordinal + 1u) {
                return false;
            }
        }
        for (std::size_t kind = 0u; kind < audit.dispatches.size(); ++kind) {
            if (audit.dispatches[kind] != audit.workgroups[kind]) {
                return false;
            }
        }
    }
    if (query.audit_.jobs[0].submission_ordinals[0]
            != shape.audit_.jobs[0].submission_ordinals[0]
        || query.audit_.jobs[0].submission_operation_identity
            == shape.audit_.jobs[0].submission_operation_identity) {
        return false;
    }

    // PREPARE, reset/burn and sealing are descriptive only. Exercise the exact
    // fixed transaction used by the production finalizer: abandoning a plan
    // models a live-gate failure; the following accepted commit reports all
    // counters, and the next accepted operation reports zero.
    if (std::any_of(
            receipts.begin(), receipts.end(),
            [](const auto& receipt) { return receipt.metrics_claimed_; })
        || seed_claimed) {
        return false;
    }
    RuntimeGpuCacheMetricsAudit maximum{
        .token_identity = 303u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = receipts[0].audit().seed_size,
        .gpu_pipeline_cache_load_ns = receipts[0].audit().seed_load_ns,
        .gpu_pipeline_creates =
            static_cast<std::uint64_t>(receipts.size()),
    };
    for (const RuntimeMetalArchivePipelineReceipt& receipt : receipts) {
        if (receipt.audit().pipeline_create_ns
            > (std::numeric_limits<std::uint64_t>::max)()
                - maximum.gpu_pipeline_creation_ns) {
            return false;
        }
        maximum.gpu_pipeline_creation_ns +=
            receipt.audit().pipeline_create_ns;
    }
    std::array<const RuntimeMetalArchivePipelineReceipt*, 11>
        receipt_pointers{};
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        receipt_pointers[i] = &receipts[i];
    }
    const auto pointers =
        std::span<const RuntimeMetalArchivePipelineReceipt* const>{
            receipt_pointers.data(), receipt_pointers.size()};
    {
        auto abandoned = runtime_detail::
            RuntimeGpuCacheMetricsClaimPlan::prepare(
                maximum.token_identity, maximum, seed_claimed, pointers);
        if (!abandoned.accepted()
            || abandoned.audit() != maximum
            || abandoned.committed()) {
            return false;
        }
    }
    if (seed_claimed
        || std::any_of(
            receipts.begin(), receipts.end(),
            [](const auto& receipt) { return receipt.metrics_claimed_; })) {
        return false;
    }
    auto first = runtime_detail::RuntimeGpuCacheMetricsClaimPlan::prepare(
        maximum.token_identity, maximum, seed_claimed, pointers);
    if (!first.accepted() || first.audit() != maximum) return false;
    first.commit(seed_claimed);
    if (!first.committed() || !seed_claimed
        || std::any_of(
            receipts.begin(), receipts.end(),
            [](const auto& receipt) { return !receipt.metrics_claimed_; })) {
        return false;
    }
    RuntimeGpuCacheMetricsAudit later_maximum = maximum;
    later_maximum.token_identity = 304u;
    auto later = runtime_detail::RuntimeGpuCacheMetricsClaimPlan::prepare(
        later_maximum.token_identity, later_maximum,
        seed_claimed, pointers);
    if (!later.accepted()
        || later.audit().token_identity != later_maximum.token_identity
        || later.audit().gpu_pipeline_cache_loads != 0u
        || later.audit().gpu_pipeline_cache_seed_bytes != 0u
        || later.audit().gpu_pipeline_cache_load_ns != 0u
        || later.audit().gpu_pipeline_creates != 0u
        || later.audit().gpu_pipeline_creation_ns != 0u) {
        return false;
    }
    later.commit(seed_claimed);
    return later.committed() && seed_claimed;
}

/// Install-once production seam.  The supervisor calls this from its runtime
/// prepare hook, before the first Runtime singleton construction.
[[nodiscard]] std::expected<void, std::string>
install_runtime_metal_archive_configuration(
    std::shared_ptr<const RuntimeMetalArchiveConfigurationImage> configuration);

[[nodiscard]] std::expected<std::shared_ptr<
    const RuntimeMetalArchiveConfigurationImage>, std::string>
make_runtime_metal_archive_configuration_image(
    std::shared_ptr<const SealedShaderCacheSeedImage> seed_image,
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid = {},
    std::uint64_t metal_device_registry_id = 0u);

[[nodiscard]] bool runtime_metal_archive_configuration_installed() noexcept;

/// The 36-byte push-constant block consumed by the tiled-DP shader. Layout is
/// pinned to the GLSL `Params` block.
struct ChainingPushConstants {
    std::uint32_t anchor_count;
    std::uint32_t range_count;
    std::int32_t max_dist_x;
    std::int32_t max_dist_y;
    std::int32_t bandwidth;
    std::int32_t max_iter;
    float gap_penalty;
    float skip_penalty;
    std::int32_t max_chain_skip;  // DOC0311: nearest-first early-exit (minimap2 max_skip); <=0 = disabled
};
static_assert(sizeof(ChainingPushConstants) == 36);
static_assert(offsetof(ChainingPushConstants, anchor_count) == 0);
static_assert(offsetof(ChainingPushConstants, range_count) == 4);
static_assert(offsetof(ChainingPushConstants, max_dist_x) == 8);
static_assert(offsetof(ChainingPushConstants, max_dist_y) == 12);
static_assert(offsetof(ChainingPushConstants, bandwidth) == 16);
static_assert(offsetof(ChainingPushConstants, max_iter) == 20);
static_assert(offsetof(ChainingPushConstants, gap_penalty) == 24);
static_assert(offsetof(ChainingPushConstants, skip_penalty) == 28);
static_assert(offsetof(ChainingPushConstants, max_chain_skip) == 32);

/// Push-constant block for the DOC0335 packed sequence generator kernel.
struct GenerateUnitsPush {
    std::uint32_t symbols_per_seq = 0;
    std::uint32_t units_per_seq = 0;
    std::uint32_t chunks_per_seq = 0;
    std::uint32_t chunk_units = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t alphabet_size = 0;
    std::uint32_t total_chunks = 0;
    std::uint32_t chunk_base = 0;
    std::uint32_t seed_lo = 0;
    std::uint32_t seed_hi = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(GenerateUnitsPush) == 48);
static_assert(std::is_standard_layout_v<GenerateUnitsPush>);

/// DOC0307 §4.5 — result of one complete minimizer sketch roundtrip. The
/// resident `minimizer_words` device buffer is moved into the caller's
/// `VulkanDeviceMinimizerStorage` for seed-lookup reuse.  Host materialisation
/// is deliberately deferred to the caller, after the arena and packed-sequence
/// transients have been released and the exact `count` is known.
struct RuntimeSketchResult {
    Buffer minimizer_words;
    std::uint64_t count = 0;
    bool overflow = false;
};

/// Whether a completed Vulkan sketch must also materialize its packed hashes
/// in a host vector.  Device-only consumers still receive the exact count and
/// resident device buffer; they do not pay for a hash download/decode.
enum class VulkanSketchHostMaterialization : std::uint8_t {
    host_and_device,
    device_only,
};

/// Operation-originated evidence for both batch sketch protocols.  The
/// established host-materialized path records count/scan/emit in one submit;
/// device-only mode separates emit so its output can use the exact scanned
/// count.  Default construction is deliberately invalid.
class RuntimeSketchBatchDispatchProof {
public:
    RuntimeSketchBatchDispatchProof() = default;
    RuntimeSketchBatchDispatchProof(
        const RuntimeSketchBatchDispatchProof&) = default;
    RuntimeSketchBatchDispatchProof& operator=(
        const RuntimeSketchBatchDispatchProof&) = default;

    [[nodiscard]] constexpr VulkanSketchHostMaterialization
    materialization() const noexcept {
        return materialization_;
    }
    [[nodiscard]] constexpr std::uint64_t count_dispatches() const noexcept {
        return count_dispatches_;
    }
    [[nodiscard]] constexpr std::uint64_t
    prefix_scan_sequences() const noexcept {
        return prefix_scan_sequences_;
    }
    [[nodiscard]] constexpr std::uint64_t
    prefix_scan_dispatches() const noexcept {
        return prefix_scan_dispatches_;
    }
    [[nodiscard]] constexpr std::uint64_t emit_dispatches() const noexcept {
        return emit_dispatches_;
    }
    [[nodiscard]] constexpr std::uint64_t
    completed_roundtrips() const noexcept {
        return completed_roundtrips_;
    }
    [[nodiscard]] constexpr bool accepted() const noexcept {
        const bool dispatch_shape_accepted = count_dispatches_ == 1u
            && prefix_scan_sequences_ == 1u
            && prefix_scan_dispatches_ == 3u
            && emit_dispatches_ == 1u;
        if (!dispatch_shape_accepted) return false;
        switch (materialization_) {
            case VulkanSketchHostMaterialization::host_and_device:
                return completed_roundtrips_ == 1u;
            case VulkanSketchHostMaterialization::device_only:
                return completed_roundtrips_ == 2u;
        }
        return false;
    }

private:
    friend class Runtime;

    VulkanSketchHostMaterialization materialization_ =
        VulkanSketchHostMaterialization::host_and_device;
    std::uint64_t count_dispatches_ = 0;
    std::uint64_t prefix_scan_sequences_ = 0;
    std::uint64_t prefix_scan_dispatches_ = 0;
    std::uint64_t emit_dispatches_ = 0;
    std::uint64_t completed_roundtrips_ = 0;
};

static_assert(!RuntimeSketchBatchDispatchProof{}.accepted());

/// Exact per-query partition of one batched query-minimizer image.
struct PhaseCSeedQueryRecordWire {
    std::uint32_t minimizer_offset = 0;
    std::uint32_t minimizer_count = 0;
    std::uint32_t query_len = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(PhaseCSeedQueryRecordWire) == 16);
static_assert(alignof(PhaseCSeedQueryRecordWire) == alignof(std::uint32_t));

/// Query-to-tile grouping supplied with the exact sketch submission. Runtime
/// derives and seals the final query records from this layout plus the device
/// count/scan output; coordinate callers cannot repartition the resident image.
struct PhaseCSketchQueryLayoutWire {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
    std::uint32_t query_len = 0;
};

/// Move-only authority for one device-only query sketch.  Only Runtime can
/// issue it, after the exact count/scan/emit submissions have completed.  The
/// authority is consumed by the persisted Shape-V5 coordinate operation; a
/// copied dispatch counter can therefore never stand in for the resident
/// query buffer that produced the operation.
class DeviceQuerySketchAuthority {
public:
    DeviceQuerySketchAuthority() = default;
    DeviceQuerySketchAuthority(const DeviceQuerySketchAuthority&) = delete;
    DeviceQuerySketchAuthority& operator=(
        const DeviceQuerySketchAuthority&) = delete;
    DeviceQuerySketchAuthority(DeviceQuerySketchAuthority&& other) noexcept {
        move_from(other);
    }
    DeviceQuerySketchAuthority& operator=(
        DeviceQuerySketchAuthority&& other) noexcept {
        if (this != &other) {
            clear();
            move_from(other);
        }
        return *this;
    }

    /// Structural, device-output coverage. This deliberately does not claim
    /// the immutable pipeline provenance required by persisted Shape-V5.
    [[nodiscard]] bool covers_device_output() const noexcept {
        const bool shape_valid = identity_ != 0u
            && runtime_ != nullptr
            && buffer_ != VK_NULL_HANDLE
            && memory_identity_ != 0u
            && buffer_bytes_ != 0u
            && allocation_bytes_ >= buffer_bytes_
            && materialization_
                == VulkanSketchHostMaterialization::device_only
            && dispatch_proof_.accepted()
            && window_size_ != 0u
            && kmer_size_ != 0u
            && minimizer_word_kind_ <= 3u
            && minimizer_words32_per_record_ != 0u
            && hash_shift_ < 64u
            && pos_bits_ < 64u
            && !query_records_.empty()
            && query_tile_counts_.size() == query_records_.size();
        if (!shape_valid) return false;
        std::uint64_t cursor = 0u;
        for (const auto& record : query_records_) {
            if (record.flags != 0u || record.query_len == 0u
                || record.minimizer_offset != cursor
                || record.minimizer_count
                    > (std::numeric_limits<std::uint64_t>::max)() - cursor) {
                return false;
            }
            cursor += record.minimizer_count;
        }
        return cursor == minimizer_count_;
    }
    /// Strong one-shot authority accepted by the persisted Shape-V5 consumer.
    [[nodiscard]] bool accepted() const noexcept {
        return covers_device_output()
            && pipeline_proof_.accepted_query_sketch();
    }
    [[nodiscard]] std::uint64_t identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        constexpr std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        if (query_records_.capacity()
                > maximum / sizeof(PhaseCSeedQueryRecordWire)
            || query_tile_counts_.capacity()
                > maximum / sizeof(std::uint32_t)) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        const std::uint64_t record_bytes =
            static_cast<std::uint64_t>(query_records_.capacity())
            * sizeof(PhaseCSeedQueryRecordWire);
        const std::uint64_t tile_count_bytes =
            static_cast<std::uint64_t>(query_tile_counts_.capacity())
            * sizeof(std::uint32_t);
        return tile_count_bytes > maximum - record_bytes
            ? maximum
            : record_bytes + tile_count_bytes;
    }

private:
    friend class Runtime;

    void clear() noexcept {
        identity_ = 0u;
        runtime_ = nullptr;
        buffer_ = VK_NULL_HANDLE;
        memory_identity_ = 0u;
        buffer_bytes_ = 0u;
        allocation_bytes_ = 0u;
        minimizer_count_ = 0u;
        window_size_ = 0u;
        kmer_size_ = 0u;
        minimizer_word_kind_ = 0u;
        minimizer_words32_per_record_ = 0u;
        hash_shift_ = 0u;
        pos_bits_ = 0u;
        materialization_ =
            VulkanSketchHostMaterialization::host_and_device;
        dispatch_proof_ = RuntimeSketchBatchDispatchProof{};
        pipeline_proof_ = RuntimePipelineOperationProof{};
        device_uuid_.fill(0u);
        query_records_.clear();
        query_tile_counts_.clear();
    }
    void move_from(DeviceQuerySketchAuthority& other) noexcept {
        identity_ = other.identity_;
        runtime_ = other.runtime_;
        buffer_ = other.buffer_;
        memory_identity_ = other.memory_identity_;
        buffer_bytes_ = other.buffer_bytes_;
        allocation_bytes_ = other.allocation_bytes_;
        minimizer_count_ = other.minimizer_count_;
        window_size_ = other.window_size_;
        kmer_size_ = other.kmer_size_;
        minimizer_word_kind_ = other.minimizer_word_kind_;
        minimizer_words32_per_record_ =
            other.minimizer_words32_per_record_;
        hash_shift_ = other.hash_shift_;
        pos_bits_ = other.pos_bits_;
        materialization_ = other.materialization_;
        dispatch_proof_ = other.dispatch_proof_;
        pipeline_proof_ = std::move(other.pipeline_proof_);
        device_uuid_ = other.device_uuid_;
        query_records_ = std::move(other.query_records_);
        query_tile_counts_ = std::move(other.query_tile_counts_);
        other.clear();
    }

    std::uint64_t identity_ = 0u;
    Runtime* runtime_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    std::uint64_t memory_identity_ = 0u;
    std::uint64_t buffer_bytes_ = 0u;
    std::uint64_t allocation_bytes_ = 0u;
    std::uint64_t minimizer_count_ = 0u;
    std::uint32_t window_size_ = 0u;
    std::uint32_t kmer_size_ = 0u;
    std::uint32_t minimizer_word_kind_ = 0u;
    std::uint32_t minimizer_words32_per_record_ = 0u;
    std::uint32_t hash_shift_ = 0u;
    std::uint32_t pos_bits_ = 0u;
    VulkanSketchHostMaterialization materialization_ =
        VulkanSketchHostMaterialization::host_and_device;
    RuntimeSketchBatchDispatchProof dispatch_proof_{};
    RuntimePipelineOperationProof pipeline_proof_{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid_{};
    std::vector<PhaseCSeedQueryRecordWire> query_records_{};
    std::vector<std::uint32_t> query_tile_counts_{};
};

static_assert(!std::is_copy_constructible_v<DeviceQuerySketchAuthority>);
static_assert(std::is_nothrow_move_constructible_v<
    DeviceQuerySketchAuthority>);

struct RuntimeSketchBatchResult {
    Buffer minimizer_words;
    std::vector<std::byte> host_minimizer_bytes;
    std::vector<std::uint32_t> tile_counts;
    std::vector<std::uint32_t> tile_offsets;
    RuntimeSketchBatchDispatchProof dispatch_proof{};
    DeviceQuerySketchAuthority query_authority{};
    std::uint64_t count = 0;
    bool overflow = false;
};

/// Physical admission plan for one batched Phase-C sketch.  The arena
/// allocation includes any larger persistent high-water buffer already owned
/// by the runtime; the output allocation uses the runtime-private Phase-B
/// buffer usage flags rather than asking callers to reproduce them.
struct PhaseCSketchBatchAllocationRequirements {
    bool valid = false;
    std::uint64_t arena_requested_bytes = 0;
    std::uint64_t arena_allocation_bytes = 0;
    std::uint64_t pessimistic_output_requested_bytes = 0;
    std::uint64_t pessimistic_output_allocation_bytes = 0;
};

/// DOC0307 §4.5 — inputs to the seed-hit lookup roundtrip. The query minimizers
/// arrive either as a resident device buffer (`device_minimizer_words`,
/// all-Vulkan sketch+seed path) or as host bytes to upload once. `slots` and
/// `positions` are the GPU-resident index buffers owned by
/// `GpuMinimizerIndexCache`; the runtime never uploads the index itself.
struct RuntimeSeedLookupInputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const Buffer* device_minimizer_words = nullptr;
    const Buffer* slots = nullptr;
    const Buffer* positions = nullptr;
    std::uint32_t slot_mask = 0;
    // Nonzero selects hash-sorted dense controls and is the exact control
    // count. Zero selects open addressing via `slot_mask`.
    std::uint32_t bucket_count = 0;
    std::uint32_t position_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint64_t target_len = 0;
    std::uint64_t target_position_offset = 0;
};

// ===========================================================================
// DOC0310 Phase C — runtime-owned arena (§7)
// ===========================================================================

/// A view into one arena buffer: `(buffer, offset, size)`. Descriptor writes
/// bind exactly this sub-range, so several logical resources can share one
/// physical arena buffer.
struct GpuArenaSlice {
    Buffer* buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    [[nodiscard]] bool valid() const noexcept { return buffer != nullptr; }
};

/// A snapshot of the three arena cursors. `rewind(mark)` restores all three.
struct GpuArenaMark {
    std::uint64_t device_cursor = 0;
    std::uint64_t upload_cursor = 0;
    std::uint64_t download_cursor = 0;
};

/// Per-runtime arena with three geometrically-grown, persistently-mapped
/// buffers (device-local, host-visible upload, host-visible download). Slices
/// are bump-allocated with alignment; `rewind` frees everything past a mark.
/// Growth is allowed only outside an active count/main pair (DOC0310 §7).
class GpuArena {
public:
    explicit GpuArena(Runtime& runtime) noexcept : runtime_(&runtime) {}

    [[nodiscard]] GpuArenaMark mark() const noexcept {
        return GpuArenaMark{device_cursor_, upload_cursor_, download_cursor_};
    }
    void rewind(GpuArenaMark mark) noexcept {
        device_cursor_ = mark.device_cursor;
        upload_cursor_ = mark.upload_cursor;
        download_cursor_ = mark.download_cursor;
    }

    /// Reserve worst-case capacity for one count/main pair before count submit,
    /// so no buffer is reallocated between the two submits. Returns false if any
    /// grow failed (caller declines Phase C).
    [[nodiscard]] bool ensure_capacity(
        std::uint64_t device_bytes, std::uint64_t upload_bytes,
        std::uint64_t download_bytes);

    [[nodiscard]] GpuArenaSlice allocate_device(
        std::uint64_t bytes, std::uint64_t alignment);
    [[nodiscard]] GpuArenaSlice allocate_upload(std::uint64_t bytes, std::uint64_t alignment);
    [[nodiscard]] GpuArenaSlice allocate_download(std::uint64_t bytes, std::uint64_t alignment);

    void bind_storage_slice(
        VkWriteDescriptorSet& write, VkDescriptorBufferInfo& info,
        std::uint32_t binding, VkDescriptorSet dst_set,
        const GpuArenaSlice& slice) const noexcept;

    [[nodiscard]] Buffer& device_buffer() noexcept { return device_; }
    [[nodiscard]] Buffer& upload_buffer() noexcept { return upload_; }
    [[nodiscard]] Buffer& download_buffer() noexcept { return download_; }

    /// Destroy every retained backing buffer after an operation has rewound all
    /// of its slices. Seed lookup is a useful full-lifetime boundary: its output
    /// is copied to owned host storage, while adopted target-index buffers are
    /// standalone allocations and therefore cannot alias this arena. Releasing
    /// the idle arena prevents large transient high-water from leaking across
    /// otherwise independent benchmark/production requests.
    [[nodiscard]] bool release_idle_capacity() noexcept;

private:
    [[nodiscard]] static std::uint64_t align_up(std::uint64_t v, std::uint64_t a) noexcept {
        return a <= 1u ? v : ((v + a - 1u) / a) * a;
    }

    Runtime* runtime_ = nullptr;
    Buffer device_;
    Buffer upload_;
    Buffer download_;
    std::uint64_t device_cursor_ = 0;
    std::uint64_t upload_cursor_ = 0;
    std::uint64_t download_cursor_ = 0;
    std::uint64_t device_capacity_ = 0;
    std::uint64_t upload_capacity_ = 0;
    std::uint64_t download_capacity_ = 0;
};

/// RAII arena reservation: holds the admission permit, marks the arena on
/// construction, and rewinds on destruction. Spans count submit + main submit.
class GpuArenaReservation {
public:
    GpuArenaReservation(GpuArena& arena, bioseq::parallelism::MemoryPermit permit) noexcept
        : arena_(&arena), permit_(std::move(permit)), mark_(arena.mark()) {}
    GpuArenaReservation(const GpuArenaReservation&) = delete;
    GpuArenaReservation& operator=(const GpuArenaReservation&) = delete;
    GpuArenaReservation(GpuArenaReservation&& other) noexcept
        : arena_(std::exchange(other.arena_, nullptr)),
          permit_(std::move(other.permit_)), mark_(other.mark_) {}
    GpuArenaReservation& operator=(GpuArenaReservation&& other) noexcept {
        if (this != &other) {
            release();
            arena_ = std::exchange(other.arena_, nullptr);
            permit_ = std::move(other.permit_);
            mark_ = other.mark_;
        }
        return *this;
    }
    ~GpuArenaReservation() { release(); }
    void release() noexcept {
        if (arena_ != nullptr) {
            arena_->rewind(mark_);
            arena_ = nullptr;
        }
    }
    [[nodiscard]] GpuArena& arena() noexcept { return *arena_; }

private:
    GpuArena* arena_ = nullptr;
    bioseq::parallelism::MemoryPermit permit_;
    GpuArenaMark mark_{};
};

/// Request-boundary cleanup for operations whose result never aliases the
/// shared Phase-C arena.  Rewinding alone leaves the arena's allocator-rounded
/// high-water live; this guard also destroys all idle backing buffers.  It is
/// intentionally independent of an admission permit so it can be installed
/// before arena growth and covers every return and exception path.
class GpuArenaTrimGuard {
public:
    explicit GpuArenaTrimGuard(GpuArena& arena) noexcept
        : arena_(&arena), mark_(arena.mark()) {}
    GpuArenaTrimGuard(const GpuArenaTrimGuard&) = delete;
    GpuArenaTrimGuard& operator=(const GpuArenaTrimGuard&) = delete;
    GpuArenaTrimGuard(GpuArenaTrimGuard&&) = delete;
    GpuArenaTrimGuard& operator=(GpuArenaTrimGuard&&) = delete;
    ~GpuArenaTrimGuard() { (void)release_now(); }

    [[nodiscard]] bool release_now() noexcept {
        if (arena_ == nullptr) {
            return true;
        }
        GpuArena* arena = std::exchange(arena_, nullptr);
        arena->rewind(mark_);
        return arena->release_idle_capacity();
    }

private:
    GpuArena* arena_ = nullptr;
    GpuArenaMark mark_{};
};

// ===========================================================================
// DOC0310 Phase C — two-submit resident frontend IO (§11)
// ===========================================================================

/// Storage-free view of a GPU target index (slots/positions buffer slices plus
/// the slot mask / counts). Lets the runtime accept an index without importing
/// `bioseq::indexing` (which would create a module cycle).
struct PhaseCTargetIndexView {
    GpuArenaSlice slots;
    GpuArenaSlice positions;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t valid = 0;
};

/// Plan describing one device radix sort over parallel (key,value) uint64 arrays.
struct GpuSortPlan {
    std::uint64_t element_count = 0;
    std::uint32_t key_bytes = 8;
    std::uint32_t value_bytes = 8;
    std::uint32_t radix_bits = 8;
    std::uint32_t pass_count = 8;
};

/// Coordinate-only chain result plus the selected predecessor trace. The trace
/// entries are anchor indices in reverse chain order, matching the
/// `phase_c_coordinate_reduce` shader scratch buffer.
struct GpuCoordinateChainTraceResult {
    GpuCoordinateOnlyResultWire result{};
    std::vector<std::uint32_t> trace{};
};

/// DOC0346 — GPU sequence sort wire structs. Byte layout pinned to the GLSL
/// `P` push block and `SeqDesc`/`SeqSortRange` declarations in shader_compiler.
struct SeqSortPush {   // 40 bytes; == GLSL pc (seq_sort kernels)
    std::uint32_t count = 0;              // row_count (key stage) / range_count (tiebreak stage)
    std::uint32_t unit_bits = 0;          // 8/16/32/64
    std::uint32_t bits_per_char = 0;      // generic only
    std::uint32_t letters_per_unit = 0;   // generic only
    std::uint32_t char_mask = 0;          // generic only
    std::uint32_t used_mask_lo = 0;
    std::uint32_t used_mask_hi = 0;
    std::uint32_t key_unit_offset = 0;       // dense scatter stage: selected key unit
    std::uint32_t sort_size = 0;           // global bitonic stage: k
    std::uint32_t sort_stride = 0;         // global bitonic stage: j
};
static_assert(sizeof(SeqSortPush) == 40);

struct SeqDescWire {   // 16 bytes; == GLSL SeqDesc. word_offset in uint32 words.
    std::uint32_t word_offset = 0;
    std::uint32_t unit_count = 0;
    std::uint32_t bp_size = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(SeqDescWire) == 16);

struct SeqSortUploadChunk {
    const void* bytes = nullptr;
    std::uint64_t byte_size = 0;
    std::uint64_t padded_byte_size = 0;
};

/// Resolved by the façade from compact_sequence_range<S>::traits.
struct SeqSortKernelParams {
    std::uint32_t alphabet_kind = 0;   // AlphabetKernelKind: dna4_nt4 -> _dna4 kinds; else _generic
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t char_mask = 0;
    std::uint64_t used_storage_mask = 0;
};

/// Result of a GPU-resident target minimizer index build (DOC0310 §12). Owns the
/// open-addressed slot table and the hash-run-ordered positions buffer. The
/// indexing layer adopts these into a `GpuResidentMinimizerIndex` outside the
/// runtime module (which must not import `bioseq::indexing`).
enum class GpuBuiltIndexFailureReason : std::uint8_t {
    none,
    invalid_shape,
    minimizer_capacity_exceeds_u32,
    memory_plan_invalid,
    device_arena_allocation_failed,
    output_capacity_overflow,
    device_buffer_allocation_failed,
    count_peak_memory_admission_denied,
    main_peak_memory_admission_denied,
    peak_allocation_exceeds_admission,
    index_invariant_failed,
};

/// Canonical query-gate identity carried with every GPU-built index.  The
/// digest accelerates rejection, but immutable exact words remain authoritative.
inline constexpr std::uint32_t kPhaseCQueryGateBits = 26u;
inline constexpr std::uint32_t kPhaseCQueryGateWordCount =
    (std::uint32_t{1} << kPhaseCQueryGateBits) / 32u;
inline constexpr std::uint32_t kPhaseCQueryGateMask =
    (std::uint32_t{1} << kPhaseCQueryGateBits) - 1u;

struct GpuBuiltIndexGateIdentity {
    bool enabled = false;
    std::uint32_t word_count = 0u;
    std::uint32_t mask = 0u;
    std::uint64_t digest_lo = 0u;
    std::uint64_t digest_hi = 0u;
    std::shared_ptr<const std::vector<std::uint32_t>> canonical_words{};

    [[nodiscard]] bool well_formed() const noexcept {
        if (!enabled) {
            return word_count == 0u && mask == 0u
                && digest_lo == 0u && digest_hi == 0u
                && !canonical_words;
        }
        return word_count == kPhaseCQueryGateWordCount
            && mask == kPhaseCQueryGateMask && canonical_words
            && canonical_words->size() == word_count;
    }

    [[nodiscard]] bool exact_equals(
        const GpuBuiltIndexGateIdentity& other) const
    {
        if (enabled != other.enabled || word_count != other.word_count
            || mask != other.mask || digest_lo != other.digest_lo
            || digest_hi != other.digest_hi) {
            return false;
        }
        if (!enabled) return true;
        if (!canonical_words || !other.canonical_words) return false;
        // Freeze guarantees there is no mutable alias. Reusing the identical
        // immutable owner is exact identity and avoids an 8 MiB cache-hit scan;
        // distinct owners still compare every word after the digest prefilter.
        if (canonical_words.get() == other.canonical_words.get()) return true;
        return *canonical_words == *other.canonical_words;
    }
};

struct GpuBuiltIndexOwnershipProof {
    std::uint64_t count_phase_admissions = 0;
    std::uint64_t main_peak_admissions = 0;
    // Exactly one device tile-count dispatch, one three-dispatch prefix scan,
    // and one eight-byte mapped control read establish T before the large
    // emit/sort allocations. No minimizer payload is materialised on the host.
    std::uint64_t exact_count_dispatches = 0;
    std::uint64_t exact_count_prefix_scan_dispatches = 0;
    std::uint64_t exact_count_control_readbacks = 0;
    // Non-empty target builds emit deterministically into prefix-owned tile
    // ranges.  The control readback validates only offsets/completion markers
    // and the device status word; it never reads hashes or positions.
    std::uint64_t deterministic_tile_emit_dispatches = 0;
    std::uint64_t atomic_append_dispatches = 0;
    std::uint64_t emit_validation_control_readbacks = 0;
    std::uint64_t counted_minimizer_count = 0;
    std::uint64_t verified_emit_count = 0;
    std::uint64_t count_emit_parity_verified = 0;
    // The caller's capacity estimate is a planning hint, never a semantic cap.
    // Exact count T may exceed it and then drives checked allocation/admission.
    std::uint64_t target_minimizer_capacity_estimate = 0;
    std::uint64_t capacity_estimate_underestimated = 0;
    // Set inside Runtime under its mutex only after the runtime scope has
    // rewound and destroyed every arena backing buffer, before the façade can
    // shrink the peak permit to resident R.
    std::uint64_t arena_trimmed_before_permit_shrink = 0;
    std::uint64_t resident_transfers = 0;
    std::uint64_t adoption_rereserve_attempts = 0;
    std::uint64_t adoption_successes = 0;
    std::uint64_t resident_bytes = 0;
    // Operation-originated dispatch/shape evidence.  Admission counters alone
    // do not prove that the query-gated GPU kernels actually ran.
    std::uint64_t gate_count_dispatches = 0;
    std::uint64_t gated_emit_dispatches = 0;
    std::uint64_t ungated_emit_dispatches = 0;
    std::uint64_t retained_minimizer_count = 0;
    std::uint64_t distinct_minimizer_count = 0;
    std::uint64_t query_gate_enabled = 0;
    std::uint64_t query_gate_word_count = 0;
    std::uint64_t query_gate_mask = 0;
    std::uint64_t query_gate_digest_lo = 0;
    std::uint64_t query_gate_digest_hi = 0;
    std::uint64_t peak_admitted_bytes = 0;
    std::uint64_t peak_observed_bytes = 0;
    // Immutable materialization evidence propagated through zero-copy adoption.
    std::uint64_t materialization_admitted_bytes = 0;
    std::uint64_t materialization_actual_allocation_bytes = 0;
    std::uint64_t materialization_fully_touched_bytes = 0;
    std::uint64_t materialization_reflected_delta_bytes = 0;
    std::uint64_t materialization_permit_released = 0;
    std::uint64_t materialization_host_visible = 0;
    std::uint64_t materialization_host_coherent = 0;
    std::uint64_t materialization_process_footprint_reflected = 0;
};

struct GpuBuiltIndex {
    // Declared first so ownership is released only after both buffers.
    bioseq::parallelism::MemoryPermit permit;
    Buffer slots;
    Buffer positions;
    GpuBuiltIndexGateIdentity gate_identity{};
    bioseq::parallelism::MemoryMaterializationCommit materialization{};
    GpuBuiltIndexOwnershipProof ownership{};
    std::uint64_t peak_admission_bytes = 0;
    std::uint64_t observed_device_peak_bytes = 0;
    bioseq::parallelism::MemoryAdmissionFailure main_peak_admission_failure{};
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t occupied_slots = 0;
    std::uint32_t position_count = 0;
    GpuBuiltIndexFailureReason failure_reason =
        GpuBuiltIndexFailureReason::none;
    bool valid = false;

    GpuBuiltIndex() = default;
    GpuBuiltIndex(const GpuBuiltIndex&) = delete;
    GpuBuiltIndex& operator=(const GpuBuiltIndex&) = delete;
    GpuBuiltIndex(GpuBuiltIndex&& other) noexcept {
        move_from(other);
    }
    GpuBuiltIndex& operator=(GpuBuiltIndex&& other) noexcept {
        if (this != &other) {
            // The destination permit owns the destination buffers' admission.
            // Destroy those buffers first; releasing the permit first creates a
            // transient live-but-unadmitted allocation window.
            positions.reset();
            slots.reset();
            permit.release();
            move_from(other);
        }
        return *this;
    }

private:
    void move_from(GpuBuiltIndex& other) noexcept {
        permit = std::move(other.permit);
        slots = std::move(other.slots);
        positions = std::move(other.positions);
        gate_identity = std::move(other.gate_identity);
        materialization = other.materialization;
        ownership = other.ownership;
        peak_admission_bytes = other.peak_admission_bytes;
        observed_device_peak_bytes = other.observed_device_peak_bytes;
        main_peak_admission_failure = other.main_peak_admission_failure;
        target_len = other.target_len;
        slot_mask = other.slot_mask;
        occupied_slots = other.occupied_slots;
        position_count = other.position_count;
        failure_reason = other.failure_reason;
        valid = other.valid;
        other.valid = false;
    }
};
static_assert(std::is_standard_layout_v<GpuBuiltIndex>);
static_assert(std::is_nothrow_move_constructible_v<GpuBuiltIndex>);
static_assert(std::is_nothrow_move_assignable_v<GpuBuiltIndex>);
static_assert(offsetof(GpuBuiltIndex, permit) < offsetof(GpuBuiltIndex, slots));
static_assert(offsetof(GpuBuiltIndex, permit) < offsetof(GpuBuiltIndex, positions));

/// DOC0345 Patch B — host-visible result of the GPU sketch+two-key-sort roundtrip.
/// Owns the device buffer holding the final sorted run plus the downloaded
/// host bytes (host/Postgres-format persistence happens on the CPU side).
struct GpuSortedPairRun {
    Buffer keys;
    Buffer values;
    std::vector<std::byte> host_key_bytes;
    std::vector<std::byte> host_value_bytes;
    std::uint64_t target_len = 0;
    std::uint32_t pair_count = 0;
    bool valid = false;
};

/// DOC0345 Patch B — device-side ping-pong views for the stable two-key pair
/// sort. Hoisted above `class Runtime` because the private method
/// `record_stable_pair_sort_by_value_then_key_locked` takes it by reference.
struct SortedPairDeviceViews {
    GpuArenaSlice keys;
    GpuArenaSlice values;
    GpuArenaSlice keys_scratch;
    GpuArenaSlice values_scratch;
    bool keys_in_scratch = false;
};

[[nodiscard]] inline const GpuArenaSlice& choose_sorted_keys(
    const SortedPairDeviceViews& views) noexcept
{
    return views.keys_in_scratch ? views.keys_scratch : views.keys;
}

[[nodiscard]] inline const GpuArenaSlice& choose_sorted_values(
    const SortedPairDeviceViews& views) noexcept
{
    return views.keys_in_scratch ? views.values_scratch : views.values;
}

/// Inputs to the GPU seed count/emit/sort roundtrip (DOC0310 §14). The query
/// minimizer words arrive resident (`device_minimizer_words`) or as host bytes;
/// the index slots/positions are the GPU-resident buffers.
struct PhaseCSeedInputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const Buffer* device_minimizer_words = nullptr;
    const Buffer* slots = nullptr;
    const Buffer* positions = nullptr;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t bucket_count = 0;  // >0 ⇒ slots is a hash-sorted dense bucket array
    std::uint64_t target_position_offset = 0;
};

/// Device-only query-minimizer input plus the authoritative encoded revision-3
/// directory. There is intentionally no host hash span in this ABI.
struct GpuPersistedDirectorySelectionInputs {
    const Buffer* device_minimizer_words = nullptr;
    const Buffer* encoded_directory = nullptr;
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t directory_entry_count = 0;
};
static_assert(std::is_standard_layout_v<GpuPersistedDirectorySelectionInputs>);

/// Opaque stable directory ordinals read back after device union/compaction.
struct GpuPersistedDirectorySelectionResult {
    std::vector<std::uint32_t> ordinals{};
    std::uint64_t device_query_minimizers_processed = 0;
};

/// Device-resident query state required by the bounded persisted format-4
/// stream.  Both buffers are outputs of the grouped32 query adapter in the
/// production graph; this ABI deliberately provides no host hash span.
struct GpuGrouped32RuntimeStateInputs {
    const Buffer* device_query_records = nullptr;
    const Buffer* device_query_minimizers = nullptr;
    std::uint32_t query_record_count = 0;
    std::uint32_t query_minimizer_count = 0;
    std::uint32_t occurrence_count = 0;
    std::uint32_t match_capacity = 0;
    std::uint32_t directory_entry_count = 0;
    std::uint32_t expected_control_groups = 0;
    std::uint32_t max_occ = 0;
    std::uint32_t kmer_size = 15;
    std::uint64_t query_identity = 0;
};

struct GpuGrouped32RawGroupInputs {
    const Buffer* descriptors = nullptr;
    const Buffer* payload = nullptr;
    std::uint32_t descriptor_count = 0;
    std::uint32_t payload_element_count = 0;
    std::uint32_t group_ordinal = 0;
    std::uint64_t receipt_identity = 0;
};

struct GpuGrouped32RuntimeControlReceipt {
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t receipt_identity = 0;
    std::uint64_t control_state_identity = 0;
    std::uint64_t matched_anchor_upper_bound = 0;
    std::uint32_t status = 0;
    bool accepted = false;
};

struct GpuGrouped32RuntimeAuthorizationReceipt {
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t receipt_identity = 0;
    std::uint64_t control_state_identity = 0;
    std::uint64_t authorization_digest = 0;
    std::uint64_t target_buffer_identity = 0;
    std::uint64_t authorized_anchor_count = 0;
    std::uint64_t anchor_bytes = 0;
    std::vector<std::uint32_t> authorized_directory_ordinals{};
    std::uint32_t status = 0;
    bool accepted = false;
};

struct GpuGrouped32RuntimePositionReceipt {
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t receipt_identity = 0;
    std::uint64_t target_buffer_identity = 0;
    std::uint64_t emitted_anchor_count = 0;
    std::uint32_t status = 0;
    bool accepted = false;
};

struct GpuGrouped32RuntimeSealReceipt {
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t receipt_identity = 0;
    std::uint64_t authorization_digest = 0;
    std::uint64_t target_buffer_identity = 0;
    std::uint64_t emitted_anchor_count = 0;
    std::uint64_t anchor_digest = 0;
    std::uint32_t status = 0;
    bool accepted = false;
};

/// Move-only owner of the target-global occurrence/match/authorization state.
/// Raw control and position slot buffers are never retained here.  The target
/// anchor allocation is installed only after the device authorization pass has
/// produced its exact count.
class GpuGrouped32RuntimeStreamState {
public:
    GpuGrouped32RuntimeStreamState() = default;
    GpuGrouped32RuntimeStreamState(
        const GpuGrouped32RuntimeStreamState&) = delete;
    GpuGrouped32RuntimeStreamState& operator=(
        const GpuGrouped32RuntimeStreamState&) = delete;
    GpuGrouped32RuntimeStreamState(
        GpuGrouped32RuntimeStreamState&&) noexcept = default;
    GpuGrouped32RuntimeStreamState& operator=(
        GpuGrouped32RuntimeStreamState&&) noexcept = default;

    [[nodiscard]] const Buffer& anchors() const noexcept { return anchors_; }
    [[nodiscard]] std::uint64_t control_state_identity() const noexcept {
        return control_state_identity_;
    }
    [[nodiscard]] std::uint64_t target_buffer_identity() const noexcept {
        return target_buffer_identity_;
    }
    [[nodiscard]] std::uint64_t authorization_digest() const noexcept {
        return authorization_digest_;
    }
    [[nodiscard]] std::uint64_t authorized_anchor_count() const noexcept {
        return authorized_anchor_count_;
    }

private:
    // Declared before owned buffers so the permit is released last.
    bioseq::parallelism::MemoryPermit permit_{};
    Buffer occurrences_{};
    Buffer matches_{};
    Buffer stream_state_{};
    Buffer authorization_flags_{};
    Buffer anchors_{};
    const Buffer* query_records_ = nullptr;
    const Buffer* query_minimizers_ = nullptr;
    std::uint32_t query_record_count_ = 0;
    std::uint32_t query_minimizer_count_ = 0;
    std::uint32_t occurrence_count_ = 0;
    std::uint32_t match_capacity_ = 0;
    std::uint32_t directory_entry_count_ = 0;
    std::uint32_t expected_control_groups_ = 0;
    std::uint32_t expected_position_groups_ = 0;
    std::uint32_t max_occ_ = 0;
    std::uint32_t kmer_size_ = 15;
    std::uint64_t query_identity_ = 0;
    std::uint64_t control_state_identity_ = 0;
    std::uint64_t target_buffer_identity_ = 0;
    std::uint64_t authorization_digest_ = 0;
    std::uint64_t authorized_anchor_count_ = 0;
    std::vector<bool> control_groups_dispatched_{};
    std::vector<bool> position_groups_dispatched_{};
    std::vector<std::uint64_t> consumed_receipt_identities_{};
    bool position_groups_bound_ = false;

    friend class Runtime;
};

/// Common query-side inputs for a batched resident seed→chain→coordinate run.
/// The query minimizer words are shared by every job in the batch. Legacy jobs
/// may supply distinct slot-table targets; an authenticated Shape-V5 direct
/// operation is intentionally restricted to one resident generation/view.
struct PhaseCSeedQueryInputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const Buffer* device_minimizer_words = nullptr;
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t window_size = 0;
    std::span<const PhaseCSeedQueryRecordWire> host_query_records{};
    const Buffer* device_query_records = nullptr;
};

/// Neutral resident view of one authenticated persisted Format-5 generation.
/// The owner keeps all three buffers alive for the complete batch.  Byte sizes
/// are logical admitted sizes; the backing raw control/position buffers may be
/// padded to whole uint32 words for GLSL access, but their representation is
/// never decoded or rebuilt by this runtime.
struct PhaseCShapeV5ResidentView {
    const Buffer* descriptors = nullptr;
    const Buffer* controls = nullptr;
    const Buffer* positions = nullptr;
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_version = 0;
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
};

enum class PhaseCSeedIndexRepresentation : std::uint8_t {
    invalid = 0,
    legacy_slot_table = 1,
    shape_v5 = 2,
};

/// One resident target-index job in a batched seed→coordinate run. Legacy
/// slot-table fields and `shape_v5` are an XOR: selecting both (or neither) is
/// invalid rather than permission to fall back to another implementation.
struct PhaseCSeedCoordinateJob {
    const Buffer* slots = nullptr;
    const Buffer* positions = nullptr;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t bucket_count = 0;
    std::uint64_t target_position_offset = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    const PhaseCShapeV5ResidentView* shape_v5 = nullptr;
};

[[nodiscard]] inline constexpr PhaseCSeedIndexRepresentation
phase_c_seed_index_representation(
    const PhaseCSeedCoordinateJob& job) noexcept
{
    const bool shape_only = job.shape_v5 != nullptr
        && job.slots == nullptr
        && job.positions == nullptr
        && job.slot_mask == 0u
        && job.position_count == 0u
        && job.bucket_count == 0u
        && job.target_position_offset == 0u;
    if (shape_only) return PhaseCSeedIndexRepresentation::shape_v5;

    const bool legacy_only = job.shape_v5 == nullptr
        && job.slots != nullptr
        && job.positions != nullptr;
    return legacy_only
        ? PhaseCSeedIndexRepresentation::legacy_slot_table
        : PhaseCSeedIndexRepresentation::invalid;
}

/// Explicit algorithm identity for the seed-to-coordinate backend.  A caller
/// selecting Vulkan tiled-DP must pass `vulkan_tiled_dp`; the approximate
/// coordinate-vote prototype is a distinct algorithm and may never be entered
/// implicitly as an optimization of tiled-DP.
enum class PhaseCSeedCoordinateAlgorithm : std::uint8_t {
    vulkan_tiled_dp = 0,
    coordinate_vote = 1,
};

/// Exact algorithm inputs bound into a persisted Shape-V5 coordinate proof.
/// Floating-point penalties are carried by bit pattern so NaNs, signed zero,
/// and every other dispatch-affecting value compare without normalization.
struct PersistedShapeV5CoordinateConfigIdentity {
    std::uint32_t kmer_size = 0u;
    std::int32_t max_occ = -1;
    PhaseCSeedCoordinateAlgorithm coordinate_algorithm =
        PhaseCSeedCoordinateAlgorithm::vulkan_tiled_dp;
    ChainingPipelineKey chain_key{};
    std::int32_t max_dist_x = 0;
    std::int32_t max_dist_y = 0;
    std::int32_t bandwidth = 0;
    std::int32_t max_iter = 0;
    std::uint32_t gap_penalty_bits = 0u;
    std::uint32_t skip_penalty_bits = 0u;
    std::int32_t max_chain_skip = 0;
    std::int32_t min_chain_score = 0;
    std::int32_t min_seed_count = 0;
    std::uint32_t probe_limit = 0u;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return kmer_size != 0u && kmer_size <= 255u
            && max_occ >= 0
            && coordinate_algorithm
                == PhaseCSeedCoordinateAlgorithm::vulkan_tiled_dp
            && chain_key.tile_size != 0u
            && chain_key.local_size != 0u
            && probe_limit != 0u;
    }
    [[nodiscard]] bool operator==(
        const PersistedShapeV5CoordinateConfigIdentity&) const = default;
};

[[nodiscard]] inline PersistedShapeV5CoordinateConfigIdentity
persisted_shape_v5_coordinate_config_identity(
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    PhaseCSeedCoordinateAlgorithm coordinate_algorithm,
    const ChainingPipelineKey& chain_key,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit) noexcept {
    return PersistedShapeV5CoordinateConfigIdentity{
        .kmer_size = kmer_size,
        .max_occ = max_occ,
        .coordinate_algorithm = coordinate_algorithm,
        .chain_key = chain_key,
        .max_dist_x = max_dist_x,
        .max_dist_y = max_dist_y,
        .bandwidth = bandwidth,
        .max_iter = max_iter,
        .gap_penalty_bits = std::bit_cast<std::uint32_t>(gap_penalty),
        .skip_penalty_bits = std::bit_cast<std::uint32_t>(skip_penalty),
        .max_chain_skip = max_chain_skip,
        .min_chain_score = min_chain_score,
        .min_seed_count = min_seed_count,
        .probe_limit = probe_limit,
    };
}

static_assert(!PersistedShapeV5CoordinateConfigIdentity{}.accepted());

struct PhaseCSeedCoordinateBatchStats {
    std::uint64_t problem_count = 0;
    std::uint64_t wave_count = 0;
    std::uint64_t seed_count_dispatches = 0;
    std::uint64_t seed_emit_dispatches = 0;
    std::uint64_t sort_dispatches = 0;
    std::uint64_t range_build_dispatches = 0;
    std::uint64_t chain_dispatches = 0;
    std::uint64_t chain_backtrack_dispatches = 0;
    std::uint64_t coordinate_vote_dispatches = 0;
};

[[nodiscard]] inline constexpr bool coordinate_result_wire_equal(
    const GpuCoordinateOnlyResultWire& lhs,
    const GpuCoordinateOnlyResultWire& rhs) noexcept {
    return lhs.query_start == rhs.query_start
        && lhs.query_end == rhs.query_end
        && lhs.target_start == rhs.target_start
        && lhs.target_end == rhs.target_end
        && lhs.match_length == rhs.match_length
        && lhs.block_length == rhs.block_length
        && lhs.chain_score == rhs.chain_score
        && lhs.seed_count == rhs.seed_count
        && lhs.is_reverse == rhs.is_reverse
        && lhs.valid == rhs.valid;
}

[[nodiscard]] inline constexpr bool coordinate_result_wire_well_formed(
    const GpuCoordinateOnlyResultWire& result,
    const std::uint64_t query_len,
    const std::uint64_t target_len) noexcept {
    if (result.valid == 0u) {
        return coordinate_result_wire_equal(
            result, GpuCoordinateOnlyResultWire{});
    }
    if (result.valid != 1u || result.is_reverse > 1u
        || result.seed_count <= 0
        || result.query_start >= result.query_end
        || result.query_end > query_len
        || result.target_start >= result.target_end
        || result.target_end > target_len
        || result.match_length > result.block_length) {
        return false;
    }
    const std::uint64_t query_span = result.query_end - result.query_start;
    const std::uint64_t target_span = result.target_end - result.target_start;
    return result.match_length <= query_span
        && result.match_length <= target_span
        && result.block_length <= query_span + target_span;
}

/// Exact caller-side identities checked while consuming one sealed operation
/// record.  These are expectations, not proof: changing any value makes the
/// one-shot consume fail.
struct PersistedShapeV5CoordinateJobExpectation {
    std::size_t problem_index = 0;
    std::uint64_t query_authority_identity = 0;
    RuntimeBufferIdentity resident_descriptors{};
    RuntimeBufferIdentity resident_controls{};
    RuntimeBufferIdentity resident_positions{};
    RuntimeBufferIdentity query_minimizer_buffer{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_version = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint64_t target_len = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    PhaseCSeedQueryRecordWire query_record{};
    GpuCoordinateOnlyResultWire result{};
    PersistedShapeV5CoordinateConfigIdentity configuration{};
};

/// Operation-local audit payload.  Runtime fills this from successful Vulkan
/// calls; it is public only so the contract can be golden/tamper tested.  It
/// cannot be converted into an operation proof by callers.
struct PersistedShapeV5CoordinateJobAudit {
    std::uint64_t operation_identity = 0;
    std::uint64_t query_authority_identity = 0;
    RuntimeBufferIdentity resident_descriptors{};
    RuntimeBufferIdentity resident_controls{};
    RuntimeBufferIdentity resident_positions{};
    RuntimeBufferIdentity query_minimizer_buffer{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_version = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint64_t target_len = 0;
    std::size_t problem_index = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    PhaseCSeedQueryRecordWire query_record{};
    GpuCoordinateOnlyResultWire result{};
    PersistedShapeV5CoordinateConfigIdentity configuration{};
    std::uint32_t query_tile_count = 0u;
    std::uint64_t query_count_dispatches = 0;
    std::uint64_t query_scan_dispatches = 0;
    std::uint64_t query_emit_dispatches = 0;
    std::uint64_t query_completed_roundtrips = 0;
    std::uint64_t direct_count_dispatches = 0;
    std::uint64_t direct_scan_dispatches = 0;
    std::uint64_t direct_emit_dispatches = 0;
    std::uint64_t anchor_count = 0;
    std::uint64_t sort_operations = 0;
    std::uint64_t range_build_dispatches = 0;
    std::uint64_t range_count = 0;
    std::uint64_t chain_dispatches = 0;
    std::uint64_t chain_backtrack_dispatches = 0;
    std::uint64_t coordinate_dispatches = 0;
    std::uint64_t count_submit_wait_identity = 0;
    std::uint64_t emit_submit_wait_identity = 0;
    std::uint64_t sort_range_submit_wait_identity = 0;
    std::uint64_t chain_coordinate_submit_wait_identity = 0;
    std::uint64_t count_status_checks = 0;
    std::uint64_t emit_status_checks = 0;
    std::uint64_t result_readbacks = 0;
    std::uint32_t status_or = 0;
    std::uint64_t coordinate_vote_dispatches = 0;
    std::uint64_t cpu_or_simd_biological_calls = 0;
    std::uint64_t recompute_calls = 0;
    std::uint64_t fallback_calls = 0;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        const bool identity_and_query = operation_identity != 0u
            && query_authority_identity != 0u
            && resident_descriptors.accepted()
            && resident_controls.accepted()
            && resident_positions.accepted()
            && query_minimizer_buffer.accepted()
            && resident_descriptors.object != resident_controls.object
            && resident_descriptors.object != resident_positions.object
            && resident_controls.object != resident_positions.object
            && query_minimizer_buffer.object
                != resident_descriptors.object
            && query_minimizer_buffer.object != resident_controls.object
            && query_minimizer_buffer.object != resident_positions.object
            && resident_descriptors.buffer_handle
                != resident_controls.buffer_handle
            && resident_descriptors.buffer_handle
                != resident_positions.buffer_handle
            && resident_controls.buffer_handle
                != resident_positions.buffer_handle
            && query_minimizer_buffer.buffer_handle
                != resident_descriptors.buffer_handle
            && query_minimizer_buffer.buffer_handle
                != resident_controls.buffer_handle
            && query_minimizer_buffer.buffer_handle
                != resident_positions.buffer_handle
            && resident_descriptors.memory_handle
                != resident_controls.memory_handle
            && resident_descriptors.memory_handle
                != resident_positions.memory_handle
            && resident_controls.memory_handle
                != resident_positions.memory_handle
            && query_minimizer_buffer.memory_handle
                != resident_descriptors.memory_handle
            && query_minimizer_buffer.memory_handle
                != resident_controls.memory_handle
            && query_minimizer_buffer.memory_handle
                != resident_positions.memory_handle
            && device_uuid
                != std::array<std::uint8_t, VK_UUID_SIZE>{}
            && descriptor_bytes != 0u
            && descriptor_bytes <= resident_descriptors.bytes
            && control_bytes != 0u
            && control_bytes <= resident_controls.bytes
            && position_bytes != 0u
            && position_bytes <= resident_positions.bytes
            && position_count != 0u
            && max_occ_cap
                != (std::numeric_limits<std::uint64_t>::max)()
            && partition_count != 0u
            && descriptor_bytes
                == static_cast<std::uint64_t>(partition_count)
                    * sizeof(GpuShapeV5ResidentPartitionDescriptor)
            && abi_version == k_gpu_shape_v5_abi_version
            && generation_id != 0u
            && source_identity != 0u
            && cold_load_identity != 0u
            && target_len != 0u
            && query_record.flags == 0u
            && configuration.accepted()
            && static_cast<std::uint64_t>(configuration.max_occ)
                <= max_occ_cap
            && query_tile_count != 0u
            && query_count_dispatches == 1u
            && query_scan_dispatches == 3u
            && query_emit_dispatches == 1u
            && query_completed_roundtrips == 2u
            && status_or == 0u
            && coordinate_vote_dispatches == 0u
            && cpu_or_simd_biological_calls == 0u
            && recompute_calls == 0u
            && fallback_calls == 0u;
        if (!identity_and_query) return false;
        if (query_record.minimizer_count == 0u) {
            // The canonical persisted-GPU contract requires a genuine
            // per-job partition-selection/seed-lookup dispatch. A query that
            // produced no minimizers cannot supply that operation evidence,
            // so it remains explicitly unsupported rather than being
            // relabelled from batch-level sketch work.
            return false;
        }
        const bool direct_front_end = direct_count_dispatches == 1u
            && direct_scan_dispatches == 3u
            && count_submit_wait_identity != 0u
            && count_status_checks == 1u;
        if (!direct_front_end) return false;
        if (anchor_count == 0u) {
            return direct_emit_dispatches == 0u
                && sort_operations == 0u
                && range_build_dispatches == 0u
                && range_count == 0u
                && chain_dispatches == 0u
                && chain_backtrack_dispatches == 0u
                && coordinate_dispatches == 0u
                && emit_submit_wait_identity == 0u
                && sort_range_submit_wait_identity == 0u
                && chain_coordinate_submit_wait_identity == 0u
                && emit_status_checks == 0u
                && result_readbacks == 0u
                && coordinate_result_wire_equal(
                    result, GpuCoordinateOnlyResultWire{});
        }
        const bool emitted_and_ranged = direct_emit_dispatches == 1u
            && sort_operations == 1u
            && range_build_dispatches == 1u
            && emit_submit_wait_identity != 0u
            && sort_range_submit_wait_identity != 0u
            && count_submit_wait_identity != emit_submit_wait_identity
            && count_submit_wait_identity
                != sort_range_submit_wait_identity
            && emit_submit_wait_identity
                != sort_range_submit_wait_identity
            && emit_status_checks == 1u;
        if (!emitted_and_ranged) return false;
        if (range_count == 0u) {
            return chain_dispatches == 0u
                && chain_backtrack_dispatches == 0u
                && coordinate_dispatches == 0u
                && chain_coordinate_submit_wait_identity == 0u
                && result_readbacks == 0u
                && coordinate_result_wire_equal(
                    result, GpuCoordinateOnlyResultWire{});
        }
        return chain_dispatches == 1u
            && chain_backtrack_dispatches == 1u
            && coordinate_dispatches == 1u
            && chain_coordinate_submit_wait_identity != 0u
            && chain_coordinate_submit_wait_identity
                != sort_range_submit_wait_identity
            && chain_coordinate_submit_wait_identity
                != emit_submit_wait_identity
            && chain_coordinate_submit_wait_identity
                != count_submit_wait_identity
            && result_readbacks == 1u
            && coordinate_result_wire_well_formed(
                result, query_record.query_len, target_len);
    }
};

/// Unforgeable consumed view of one accepted audit record.
class PersistedShapeV5CoordinateJobEvidence {
public:
    PersistedShapeV5CoordinateJobEvidence() = default;
    PersistedShapeV5CoordinateJobEvidence(
        const PersistedShapeV5CoordinateJobEvidence&) = delete;
    PersistedShapeV5CoordinateJobEvidence& operator=(
        const PersistedShapeV5CoordinateJobEvidence&) = delete;
    PersistedShapeV5CoordinateJobEvidence(
        PersistedShapeV5CoordinateJobEvidence&& other) noexcept {
        move_from(other);
    }
    PersistedShapeV5CoordinateJobEvidence& operator=(
        PersistedShapeV5CoordinateJobEvidence&& other) noexcept {
        if (this != &other) {
            audit_ = {};
            pipeline_operation_identity_ = 0u;
            issued_ = false;
            move_from(other);
        }
        return *this;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return issued_ && pipeline_operation_identity_ != 0u
            && audit_.accepted();
    }
    [[nodiscard]] const PersistedShapeV5CoordinateJobAudit& audit()
        const noexcept {
        return audit_;
    }

private:
    friend class PersistedShapeV5CoordinateOperationProof;

    void move_from(PersistedShapeV5CoordinateJobEvidence& other) noexcept {
        audit_ = other.audit_;
        issued_ = other.issued_;
        pipeline_operation_identity_ = other.pipeline_operation_identity_;
        other.audit_ = {};
        other.issued_ = false;
        other.pipeline_operation_identity_ = 0u;
    }

    PersistedShapeV5CoordinateJobAudit audit_{};
    std::uint64_t pipeline_operation_identity_ = 0u;
    bool issued_ = false;
};

struct PersistedShapeV5CoordinateOperationAcceptanceAudit {
    std::uint64_t operation_identity = 0u;
    std::uint64_t expected_job_count = 0u;
    std::uint64_t consumed_count = 0u;
    std::uint64_t accepted_count = 0u;
    std::uint64_t burned_count = 0u;
    std::uint64_t query_pipeline_receipt_count = 0u;
    std::uint64_t shape_pipeline_receipt_count = 0u;
    std::uint64_t committed_submission_count = 0u;
    std::uint64_t seed_receipt_identity = 0u;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return operation_identity != 0u && expected_job_count != 0u
            && consumed_count == expected_job_count
            && accepted_count == expected_job_count
            && burned_count == 0u
            && query_pipeline_receipt_count == 3u
            && shape_pipeline_receipt_count >= 7u
            && shape_pipeline_receipt_count <= 10u
            && committed_submission_count >= 3u
            && seed_receipt_identity != 0u;
    }
    [[nodiscard]] constexpr bool operator==(
        const PersistedShapeV5CoordinateOperationAcceptanceAudit&)
        const noexcept = default;
};

/// Allocation-free pre-finalizer view of the exact operation authority fields
/// later copied into the SQL evidence ledger.
struct PersistedShapeV5CoordinateOperationEvidencePreview {
    PersistedShapeV5CoordinateOperationAcceptanceAudit acceptance{};
    std::uint64_t query_pipeline_operation_identity = 0u;
    std::uint64_t shape_pipeline_operation_identity = 0u;
    std::array<std::uint64_t, 2> query_submission_ordinals{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    RuntimeMetalArchiveProcessQualification cache_qualification{};
    RuntimeGpuCacheMetricsAudit maximum_cache_metrics{};

    [[nodiscard]] bool accepted() const noexcept {
        return acceptance.accepted()
            && query_pipeline_operation_identity != 0u
            && shape_pipeline_operation_identity
                == acceptance.operation_identity
            && query_pipeline_operation_identity
                != shape_pipeline_operation_identity
            && query_submission_ordinals[0] != 0u
            && query_submission_ordinals[1] != 0u
            && query_submission_ordinals[0]
                != query_submission_ordinals[1]
            && api_version != 0u && driver_version != 0u
            && seed_size != 0u && manifest_checksum != 0u
            && cache_qualification.accepted()
            && cache_qualification.vulkan_device_uuid == device_uuid
            && cache_qualification.api_version == api_version
            && cache_qualification.driver_version == driver_version
            && cache_qualification.vendor_id == vendor_id
            && cache_qualification.device_id == device_id
            && cache_qualification.pipeline_cache_uuid
                == pipeline_cache_uuid
            && cache_qualification.seed_sha256 == seed_sha256
            && cache_qualification.seed_size == seed_size
            && cache_qualification.manifest_checksum == manifest_checksum
            && maximum_cache_metrics.accepted()
            && maximum_cache_metrics.token_identity
                == acceptance.operation_identity
            && std::any_of(
                device_uuid.begin(), device_uuid.end(),
                [](const std::uint8_t value) { return value != 0u; })
            && std::any_of(
                pipeline_cache_uuid.begin(), pipeline_cache_uuid.end(),
                [](const std::uint8_t value) { return value != 0u; })
            && std::any_of(
                seed_sha256.begin(), seed_sha256.end(),
                [](const std::uint8_t value) { return value != 0u; });
    }
    [[nodiscard]] constexpr bool operator==(
        const PersistedShapeV5CoordinateOperationEvidencePreview&)
        const noexcept = default;
};

/// Exact full-stage job payload plus its structured Shape submission receipts.
struct PersistedShapeV5CoordinateJobEvidencePreview {
    PersistedShapeV5CoordinateJobAudit audit{};
    std::uint64_t shape_pipeline_operation_identity = 0u;
    std::array<std::uint64_t, 4> shape_submission_ordinals{};

    [[nodiscard]] bool accepted() const noexcept {
        if (!audit.accepted() || audit.anchor_count == 0u
            || audit.range_count == 0u || audit.result.valid != 1u
            || shape_pipeline_operation_identity == 0u) {
            return false;
        }
        for (std::size_t i = 0u;
             i < shape_submission_ordinals.size(); ++i) {
            if (shape_submission_ordinals[i] == 0u) return false;
            for (std::size_t prior = 0u; prior < i; ++prior) {
                if (shape_submission_ordinals[prior]
                        == shape_submission_ordinals[i]) {
                    return false;
                }
            }
        }
        return true;
    }
};

class PersistedShapeV5CoordinateAcceptedOperation {
public:
    PersistedShapeV5CoordinateAcceptedOperation() = default;
    PersistedShapeV5CoordinateAcceptedOperation(
        const PersistedShapeV5CoordinateAcceptedOperation&) = delete;
    PersistedShapeV5CoordinateAcceptedOperation& operator=(
        const PersistedShapeV5CoordinateAcceptedOperation&) = delete;
    PersistedShapeV5CoordinateAcceptedOperation(
        PersistedShapeV5CoordinateAcceptedOperation&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          environment_(std::exchange(other.environment_, {})),
          cache_metrics_(std::move(other.cache_metrics_)),
          issued_(std::exchange(other.issued_, false)) {}
    PersistedShapeV5CoordinateAcceptedOperation& operator=(
        PersistedShapeV5CoordinateAcceptedOperation&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            environment_ = std::exchange(other.environment_, {});
            cache_metrics_ = std::move(other.cache_metrics_);
            issued_ = std::exchange(other.issued_, false);
        }
        return *this;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return issued_ && audit_.accepted() && environment_.accepted()
            && cache_metrics_.accepted();
    }
    [[nodiscard]] const PersistedShapeV5CoordinateOperationAcceptanceAudit&
    audit() const noexcept { return audit_; }
    [[nodiscard]] const RuntimeGpuExecutionEnvironmentBinding&
    environment_binding() const noexcept { return environment_; }
    [[nodiscard]] const RuntimeGpuCacheMetricsAudit&
    cache_metrics_audit() const noexcept { return cache_metrics_.audit(); }
    [[nodiscard]] RuntimeGpuCacheMetricsDelta
    take_cache_metrics_delta() noexcept {
        return std::move(cache_metrics_);
    }

private:
    PersistedShapeV5CoordinateOperationAcceptanceAudit audit_{};
    RuntimeGpuExecutionEnvironmentBinding environment_{};
    RuntimeGpuCacheMetricsDelta cache_metrics_{};
    bool issued_ = false;
    friend class Runtime;
};

static_assert(!std::is_copy_constructible_v<
    PersistedShapeV5CoordinateAcceptedOperation>);
static_assert(std::is_nothrow_move_constructible_v<
    PersistedShapeV5CoordinateAcceptedOperation>);

/// Runtime-sealed, move-only proof for one Shape-V5 coordinate batch.  Each
/// logical job can be consumed exactly once and only with matching resident,
/// query-record and result identities.
class PersistedShapeV5CoordinateOperationProof {
public:
    PersistedShapeV5CoordinateOperationProof() = default;
    PersistedShapeV5CoordinateOperationProof(
        const PersistedShapeV5CoordinateOperationProof&) = delete;
    PersistedShapeV5CoordinateOperationProof& operator=(
        const PersistedShapeV5CoordinateOperationProof&) = delete;
    PersistedShapeV5CoordinateOperationProof(
        PersistedShapeV5CoordinateOperationProof&& other) noexcept {
        move_from(other);
    }
    PersistedShapeV5CoordinateOperationProof& operator=(
        PersistedShapeV5CoordinateOperationProof&& other) noexcept {
        if (this != &other) {
            release_records();
            query_pipeline_proof_ = {};
            shape_pipeline_proof_ = {};
            move_from(other);
        }
        return *this;
    }

    [[nodiscard]] bool sealed() const noexcept {
        return sealed_ && operation_identity_ != 0u;
    }
    [[nodiscard]] std::size_t job_count() const noexcept {
        return sealed() ? records_.size() : 0u;
    }
    [[nodiscard]] std::size_t consumed_count() const noexcept {
        return sealed() ? accepted_count_ + burned_count_ : 0u;
    }
    [[nodiscard]] std::size_t accepted_count() const noexcept {
        return sealed() ? accepted_count_ : 0u;
    }
    [[nodiscard]] std::size_t burned_count() const noexcept {
        return sealed() ? burned_count_ : 0u;
    }
    [[nodiscard]] bool fully_consumed() const noexcept {
        return sealed()
            && !records_.empty()
            && std::all_of(
                records_.begin(),
                records_.end(), [](const auto& record) {
                    return record.state != RecordState::fresh;
                });
    }
    [[nodiscard]] bool all_jobs_accepted() const noexcept {
        return sealed() && fully_consumed()
            && accepted_count_ == records_.size()
            && burned_count_ == 0u && causal_proofs_accepted();
    }
    [[nodiscard]] RuntimeGpuExecutionEnvironmentBinding
    environment_preview() const noexcept {
        return preview_environment();
    }
    [[nodiscard]] RuntimeGpuCacheMetricsAudit
    cache_metrics_preview() const noexcept {
        return maximum_cache_metrics();
    }
    [[nodiscard]] PersistedShapeV5CoordinateOperationEvidencePreview
    operation_evidence_preview() const noexcept {
        PersistedShapeV5CoordinateOperationEvidencePreview result{};
        if (!all_jobs_accepted()
            || !query_pipeline_proof_.accepted_query_sketch()
            || !shape_pipeline_proof_.accepted_shape_v5_coordinate()) {
            return result;
        }
        const RuntimePipelineOperationAudit& query =
            query_pipeline_proof_.audit();
        const RuntimePipelineOperationAudit& shape =
            shape_pipeline_proof_.audit();
        if (query.jobs.size() != 1u
            || query.jobs.front().committed_submissions != 2u
            || shape.committed_submissions
                > (std::numeric_limits<std::uint64_t>::max)()
                    - query.committed_submissions) {
            return result;
        }
        result.acceptance =
            PersistedShapeV5CoordinateOperationAcceptanceAudit{
                .operation_identity = operation_identity_,
                .expected_job_count = static_cast<std::uint64_t>(
                    records_.size()),
                .consumed_count = static_cast<std::uint64_t>(
                    consumed_count()),
                .accepted_count = static_cast<std::uint64_t>(
                    accepted_count()),
                .burned_count = static_cast<std::uint64_t>(
                    burned_count()),
                .query_pipeline_receipt_count =
                    static_cast<std::uint64_t>(query.prepared_count),
                .shape_pipeline_receipt_count =
                    static_cast<std::uint64_t>(shape.prepared_count),
                .committed_submission_count = query.committed_submissions
                    + shape.committed_submissions,
                .seed_receipt_identity = query.seed_receipt_identity,
            };
        result.query_pipeline_operation_identity =
            query.operation_identity;
        result.shape_pipeline_operation_identity =
            shape.operation_identity;
        result.query_submission_ordinals[0] =
            query.jobs.front().submission_ordinals[0];
        result.query_submission_ordinals[1] =
            query.jobs.front().submission_ordinals[1];
        result.device_uuid = query.device_uuid;
        result.api_version = query.api_version;
        result.driver_version = query.driver_version;
        result.vendor_id = query.vendor_id;
        result.device_id = query.device_id;
        result.pipeline_cache_uuid = query.pipeline_cache_uuid;
        result.seed_sha256 = query.seed_sha256;
        result.seed_size = query.seed_size;
        result.manifest_checksum = query.manifest_checksum;
        if (query.cache_qualification != shape.cache_qualification) {
            return {};
        }
        result.cache_qualification = query.cache_qualification;
        result.maximum_cache_metrics = maximum_cache_metrics();
        return result.accepted()
            ? result
            : PersistedShapeV5CoordinateOperationEvidencePreview{};
    }
    [[nodiscard]] std::optional<
        PersistedShapeV5CoordinateJobEvidencePreview>
    job_evidence_preview(const std::size_t problem_index) const noexcept {
        if (!all_jobs_accepted() || problem_index >= records_.size()
            || !shape_pipeline_proof_.accepted_shape_v5_coordinate()) {
            return std::nullopt;
        }
        const RuntimePipelineOperationAudit& shape =
            shape_pipeline_proof_.audit();
        if (problem_index >= shape.jobs.size()) return std::nullopt;
        const RuntimePipelineJobUseAudit& use = shape.jobs[problem_index];
        if (use.committed_submissions != 4u
            || use.submission_operation_identity
                != shape.operation_identity) {
            return std::nullopt;
        }
        PersistedShapeV5CoordinateJobEvidencePreview result{
            .audit = records_[problem_index].audit,
            .shape_pipeline_operation_identity =
                use.submission_operation_identity,
            .shape_submission_ordinals = use.submission_ordinals,
        };
        return result.accepted()
            ? std::optional<
                PersistedShapeV5CoordinateJobEvidencePreview>{result}
            : std::nullopt;
    }

    /// Contract-only test hook.  It returns no proof or evidence and therefore
    /// grants no runtime authority; it only lets the portable suite exercise
    /// private sealing, move invalidation, tamper burning, and one-shot consume.
    [[nodiscard]] static bool self_test_one_shot_contract(
        const std::array<PersistedShapeV5CoordinateJobAudit, 2>& audits,
        const std::array<PersistedShapeV5CoordinateJobExpectation, 2>&
            expectations) {
        if (audits[0].operation_identity == 0u
            || audits[0].operation_identity != audits[1].operation_identity
            || audits[0].problem_index != 0u
            || audits[1].problem_index != 1u
            || !audits[0].accepted()
            || !audits[1].accepted()) {
            return false;
        }
        PersistedShapeV5CoordinateOperationProof issued{};
        issued.operation_identity_ = audits[0].operation_identity;
        try {
            issued.admit_records(2u);
        } catch (...) {
            return false;
        }
        issued.records_[0].audit = audits[0];
        issued.records_[1].audit = audits[1];
        issued.test_causal_accepted_ = true;
        issued.sealed_ = true;

        PersistedShapeV5CoordinateOperationProof proof{std::move(issued)};
        if (issued.sealed() || issued.job_count() != 0u
            || issued.records_capacity_permit_
            || !proof.records_capacity_permit_
            || !proof.sealed() || proof.job_count() != 2u) {
            return false;
        }
        auto first = proof.consume(expectations[0]);
        if (!first.has_value() || !first->accepted()) return false;
        if (proof.fully_consumed()) return false;
        PersistedShapeV5CoordinateJobEvidence moved_first{
            std::move(*first)};
        if (first->accepted() || !moved_first.accepted()) return false;
        if (proof.consume(expectations[0]).has_value()) return false;

        auto tampered_second = expectations[1];
        tampered_second.source_identity ^= 1u;
        if (proof.consume(tampered_second).has_value()) return false;
        if (proof.consume(expectations[1]).has_value()) return false;
        if (!proof.fully_consumed() || proof.accepted_count() != 1u
            || proof.burned_count() != 1u
            || proof.all_jobs_accepted()) return false;

        PersistedShapeV5CoordinateOperationProof all_valid{};
        all_valid.operation_identity_ = audits[0].operation_identity;
        try {
            all_valid.admit_records(2u);
        } catch (...) {
            return false;
        }
        all_valid.records_[0].audit = audits[0];
        all_valid.records_[1].audit = audits[1];
        all_valid.test_causal_accepted_ = true;
        all_valid.sealed_ = true;
        auto valid_first = all_valid.consume(expectations[0]);
        auto valid_second = all_valid.consume(expectations[1]);
        return valid_first.has_value() && valid_first->accepted()
            && valid_second.has_value() && valid_second->accepted()
            && !all_valid.consume(expectations[0]).has_value()
            && !all_valid.consume(expectations[1]).has_value()
            && all_valid.fully_consumed()
            && all_valid.accepted_count() == 2u
            && all_valid.burned_count() == 0u
            && all_valid.records_capacity_permit_
            && all_valid.all_jobs_accepted();
    }

    [[nodiscard]] std::optional<PersistedShapeV5CoordinateJobEvidence>
    consume(const PersistedShapeV5CoordinateJobExpectation& expected) noexcept {
        if (!sealed() || expected.problem_index >= records_.size()) {
            return std::nullopt;
        }
        Record& record = records_[expected.problem_index];
        if (record.state != RecordState::fresh) return std::nullopt;
        const auto& audit = record.audit;
        const auto& query = expected.query_record;
        const bool identities_match = audit.problem_index
                == expected.problem_index
            && audit.operation_identity == operation_identity_
            && audit.query_authority_identity
                == expected.query_authority_identity
            && audit.resident_descriptors == expected.resident_descriptors
            && audit.resident_controls == expected.resident_controls
            && audit.resident_positions == expected.resident_positions
            && audit.query_minimizer_buffer
                == expected.query_minimizer_buffer
            && audit.device_uuid == expected.device_uuid
            && audit.descriptor_bytes == expected.descriptor_bytes
            && audit.control_bytes == expected.control_bytes
            && audit.position_bytes == expected.position_bytes
            && audit.position_count == expected.position_count
            && audit.max_occ_cap == expected.max_occ_cap
            && audit.partition_count == expected.partition_count
            && audit.abi_version == expected.abi_version
            && audit.generation_id == expected.generation_id
            && audit.source_identity == expected.source_identity
            && audit.cold_load_identity == expected.cold_load_identity
            && audit.target_len == expected.target_len
            && audit.query_index == expected.query_index
            && audit.logical_target_row == expected.logical_target_row
            && audit.logical_query_ordinal
                == expected.logical_query_ordinal
            && audit.query_record.minimizer_offset
                == query.minimizer_offset
            && audit.query_record.minimizer_count
                == query.minimizer_count
            && audit.query_record.query_len == query.query_len
            && audit.query_record.flags == query.flags
            && coordinate_result_wire_equal(audit.result, expected.result)
            && audit.configuration == expected.configuration;
        if (!identities_match || !audit.accepted()
            || !causal_proofs_accepted()) {
            record.state = RecordState::burned;
            ++burned_count_;
            return std::nullopt;
        }
        record.state = RecordState::accepted;
        ++accepted_count_;
        PersistedShapeV5CoordinateJobEvidence evidence{};
        evidence.audit_ = audit;
        evidence.pipeline_operation_identity_ = operation_identity_;
        evidence.issued_ = true;
        return evidence;
    }

private:
    friend class Runtime;

    enum class RecordState : std::uint8_t {
        fresh,
        accepted,
        burned,
    };

    struct Record {
        PersistedShapeV5CoordinateJobAudit audit{};
        RecordState state = RecordState::fresh;
    };

    struct PipelineReceiptUnion {
        std::array<ShaderVariantKey,
                   k_runtime_pipeline_prepared_slots> keys{};
        std::array<const RuntimeMetalArchivePipelineReceipt*,
                   k_runtime_pipeline_prepared_slots> receipts{};
        std::size_t count = 0u;
        bool valid = false;
    };

    [[nodiscard]] PipelineReceiptUnion receipt_union() const noexcept {
        PipelineReceiptUnion result{};
        if (!sealed_ || test_causal_accepted_
            || !query_pipeline_proof_.accepted_query_sketch()
            || !shape_pipeline_proof_.accepted_shape_v5_coordinate()) {
            return result;
        }
        const auto merge = [&result](
            const RuntimePipelineOperationProof& operation) {
            for (std::size_t i = 0u;
                 i < operation.audit_.prepared_count; ++i) {
                std::size_t slot = result.count;
                for (std::size_t existing = 0u;
                     existing < result.count; ++existing) {
                    if (result.keys[existing]
                            == operation.audit_.prepared[i].shader_key) {
                        slot = existing;
                        break;
                    }
                }
                if (slot == result.count) {
                    if (result.count == result.keys.size()) return false;
                    result.keys[result.count] =
                        operation.audit_.prepared[i].shader_key;
                    result.receipts[result.count] =
                        operation.receipts_[i];
                    ++result.count;
                } else if (result.receipts[slot]
                           != operation.receipts_[i]) {
                    return false;
                }
            }
            return true;
        };
        if (!merge(query_pipeline_proof_)
            || !merge(shape_pipeline_proof_)
            || result.count == 0u
            || result.count
                != shape_pipeline_proof_.audit_.prepared_count + 1u) {
            return {};
        }
        result.valid = true;
        return result;
    }

    [[nodiscard]] RuntimeGpuExecutionEnvironmentBinding
    preview_environment() const noexcept {
        const PipelineReceiptUnion union_receipts = receipt_union();
        if (!union_receipts.valid) return {};
        const RuntimePipelineOperationAudit& query =
            query_pipeline_proof_.audit_;
        const RuntimePipelineOperationAudit& shape =
            shape_pipeline_proof_.audit_;
        if (query.device_uuid != shape.device_uuid
            || query.api_version != shape.api_version
            || query.driver_version != shape.driver_version
            || query.vendor_id != shape.vendor_id
            || query.device_id != shape.device_id
            || query.pipeline_cache_uuid != shape.pipeline_cache_uuid
            || query.seed_sha256 != shape.seed_sha256
            || query.seed_size != shape.seed_size
            || query.manifest_checksum != shape.manifest_checksum
            || query.seed_receipt_identity
                != shape.seed_receipt_identity
            || query.archive_set != shape.archive_set
            || query.cache_qualification != shape.cache_qualification) {
            return {};
        }
        VulkanFeatureSet identity{};
        identity.api_version = query.api_version;
        identity.driver_version = query.driver_version;
        identity.vendor_id = query.vendor_id;
        identity.device_id = query.device_id;
        identity.pipeline_cache_uuid = query.pipeline_cache_uuid;
        RuntimeGpuExecutionEnvironmentBinding result{
            .gpu_environment_present = true,
            .gpu_device_uuid = query.device_uuid,
            .gpu_driver_identity =
                canonical_vulkan_driver_identity(identity),
            .cache_policy = RuntimePipelineCachePolicyEvidence::
                immutable_preseed_all_hit,
            .seed_sha256 = query.seed_sha256,
            .seed_size = query.seed_size,
            .manifest_checksum = query.manifest_checksum,
            .pipeline_cache_all_hit = true,
        };
        return result.accepted() ? result
                                 : RuntimeGpuExecutionEnvironmentBinding{};
    }

    [[nodiscard]] RuntimeGpuCacheMetricsAudit
    maximum_cache_metrics() const noexcept {
        const PipelineReceiptUnion union_receipts = receipt_union();
        if (!union_receipts.valid) return {};
        RuntimeGpuCacheMetricsAudit result{
            .token_identity = operation_identity_,
            .gpu_pipeline_cache_loads = 1u,
            .gpu_pipeline_cache_seed_bytes =
                query_pipeline_proof_.audit_.seed_size,
        };
        const RuntimeMetalArchivePipelineReceipt* first =
            union_receipts.receipts[0];
        if (first == nullptr || !first->accepted()) return {};
        result.gpu_pipeline_cache_load_ns = first->audit().seed_load_ns;
        for (std::size_t i = 0u; i < union_receipts.count; ++i) {
            const RuntimeMetalArchivePipelineReceipt* receipt =
                union_receipts.receipts[i];
            if (receipt == nullptr || !receipt->accepted()
                || receipt->audit().seed_load_ns
                    != result.gpu_pipeline_cache_load_ns
                || result.gpu_pipeline_creates
                    == (std::numeric_limits<std::uint64_t>::max)()
                || receipt->audit().pipeline_create_ns
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - result.gpu_pipeline_creation_ns) {
                return {};
            }
            ++result.gpu_pipeline_creates;
            result.gpu_pipeline_creation_ns +=
                receipt->audit().pipeline_create_ns;
        }
        return result.accepted() ? result : RuntimeGpuCacheMetricsAudit{};
    }

    void admit_records(std::size_t record_count) {
        using bioseq::parallelism::saturating_mul;
        if (record_count == 0u || !records_.empty()
            || records_capacity_permit_
            || record_count > records_.max_size()) {
            throw VulkanRuntimeError(
                "persisted Shape-V5 operation record shape is invalid");
        }
        const std::uint64_t logical = saturating_mul(
            static_cast<std::uint64_t>(record_count), sizeof(Record));
        const std::uint64_t peak = saturating_mul(
            logical,
            bioseq::parallelism::kVectorPreallocationHeadroomFactor);
        if (logical == (std::numeric_limits<std::uint64_t>::max)()
            || peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw VulkanRuntimeError(
                "persisted Shape-V5 operation record admission saturated");
        }
        bioseq::parallelism::MemoryAdmissionFailure failure{};
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(peak, &failure);
        if (!permit) {
            throw VulkanRuntimeError(shader_cache_message(
                "persisted Shape-V5 operation record admission denied: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(
                    failure.reason),
                " requested=", failure.requested_bytes));
        }
        try {
            records_.resize(record_count);
        } catch (const std::bad_alloc&) {
            std::vector<Record> empty;
            records_.swap(empty);
            throw VulkanRuntimeError(
                "persisted Shape-V5 operation record allocation failed after admission");
        }
        const std::uint64_t retained = saturating_mul(
            static_cast<std::uint64_t>(records_.capacity()),
            sizeof(Record));
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || retained > permit->bytes()) {
            std::vector<Record> empty;
            records_.swap(empty);
            throw VulkanRuntimeError(
                "persisted Shape-V5 operation records exceeded admitted capacity");
        }
        permit->shrink_to(retained);
        records_capacity_permit_ = std::move(*permit);
    }

    void release_records() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            records_, records_capacity_permit_);
    }

    [[nodiscard]] bool causal_proofs_accepted() const noexcept {
        return test_causal_accepted_
            || (query_pipeline_proof_.accepted_query_sketch()
                && shape_pipeline_proof_.accepted_shape_v5_coordinate());
    }

    void move_from(PersistedShapeV5CoordinateOperationProof& other) noexcept {
        operation_identity_ = other.operation_identity_;
        records_ = std::move(other.records_);
        records_capacity_permit_ = std::move(
            other.records_capacity_permit_);
        query_pipeline_proof_ = std::move(other.query_pipeline_proof_);
        shape_pipeline_proof_ = std::move(other.shape_pipeline_proof_);
        accepted_count_ = other.accepted_count_;
        burned_count_ = other.burned_count_;
        test_causal_accepted_ = other.test_causal_accepted_;
        sealed_ = other.sealed_;
        other.operation_identity_ = 0u;
        other.records_.clear();
        other.accepted_count_ = 0u;
        other.burned_count_ = 0u;
        other.test_causal_accepted_ = false;
        other.sealed_ = false;
    }

    std::uint64_t operation_identity_ = 0u;
    // Declared before records_: record capacity is destroyed before release.
    bioseq::parallelism::MemoryPermit records_capacity_permit_{};
    std::vector<Record> records_{};
    RuntimePipelineOperationProof query_pipeline_proof_{};
    RuntimePipelineOperationProof shape_pipeline_proof_{};
    std::size_t accepted_count_ = 0u;
    std::size_t burned_count_ = 0u;
    bool test_causal_accepted_ = false;
    bool sealed_ = false;
};

static_assert(!std::is_copy_constructible_v<
    PersistedShapeV5CoordinateOperationProof>);
static_assert(std::is_nothrow_move_constructible_v<
    PersistedShapeV5CoordinateOperationProof>);
static_assert(!PersistedShapeV5CoordinateJobAudit{}.accepted());

namespace runtime_detail {

/// Fixed final-publication payload prepared before the immutable live gate.
/// Dropping this object without committing its claim plan has no side effects.
struct PersistedShapeV5CoordinateFinalizationPlan {
    RuntimeGpuCacheMetricsClaimPlan cache_claim_plan{};
    RuntimeGpuExecutionEnvironmentBinding environment{};
    PersistedShapeV5CoordinateOperationAcceptanceAudit acceptance{};
    bool valid = false;

    [[nodiscard]] bool accepted() const noexcept {
        return valid && cache_claim_plan.accepted()
            && environment.accepted() && acceptance.accepted();
    }
};

static_assert(!std::is_copy_constructible_v<
    PersistedShapeV5CoordinateFinalizationPlan>);
static_assert(std::is_nothrow_move_constructible_v<
    PersistedShapeV5CoordinateFinalizationPlan>);

}  // namespace runtime_detail

struct PhaseCSeedCoordinateBatchRun {
    std::vector<GpuCoordinateOnlyResultWire> results{};
    PhaseCSeedCoordinateBatchStats stats{};
    PersistedShapeV5CoordinateOperationProof operation_proof{};
    struct ShapeV5DirectRouteEvidence {
        bool selected = false;
        bool direct_count_scan_emit_completed = false;
        std::uint64_t problem_count = 0;
        std::uint64_t generation_id = 0;
        std::uint64_t source_identity = 0;
        std::uint64_t cold_load_identity = 0;
        std::uint64_t count_dispatches = 0;
        std::uint64_t emit_dispatches = 0;
        std::uint64_t count_status_checks = 0;
        std::uint64_t emit_status_checks = 0;
        std::uint32_t status_or = 0;
        std::uint64_t cpu_or_simd_fallbacks = 0;
    } shape_v5_direct{};
};

/// The Phase A chaining runtime. One process-wide instance (`Runtime::instance`)
/// owns the device and serializes GPU queue access with `mu_`.
class Runtime {
public:
    Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime();

    [[nodiscard]] static Runtime& instance();
    [[nodiscard]] const VulkanFeatureSet& features() const noexcept { return features_; }
    [[nodiscard]] bool usable() const noexcept { return features_.available; }
    [[nodiscard]] const std::string& initialization_error() const noexcept {
        return init_error_;
    }
    [[nodiscard]] std::uint64_t chaining_dispatch_count() const noexcept {
        return chaining_dispatch_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t generate_units_dispatch_count() const noexcept {
        return generate_units_dispatch_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t pipeline_cache_metadata_reserved_bytes() {
        std::lock_guard<std::mutex> guard(mu_);
        return pipeline_cache_metadata_permit_.bytes();
    }
    [[nodiscard]] PersistedShapeV5CoordinateAcceptedOperation
    finalize_persisted_shape_v5_coordinate_operation(
        PersistedShapeV5CoordinateOperationProof&& proof,
        const PersistedShapeV5CoordinateOperationEvidencePreview&
            expected_preview);
    [[nodiscard]] static bool
    self_test_persisted_shape_v5_finalizer_preclaim_contract(
        RuntimeMetalArchivePipelineAudit base_audit);
    [[nodiscard]] static bool self_test_gpu_cache_partial_delta_contract(
        RuntimeMetalArchivePipelineAudit base_audit);

    [[nodiscard]] Buffer create_host_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    [[nodiscard]] Buffer create_device_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    [[nodiscard]] std::uint64_t buffer_allocation_requirement(
        std::uint64_t requested_bytes,
        VkBufferUsageFlags usage);
    [[nodiscard]] std::uint64_t phase_c_arena_allocation_requirement(
        std::uint64_t requested_bytes);
    [[nodiscard]] PhaseCSketchBatchAllocationRequirements
    phase_c_sketch_batch_allocation_requirements(
        std::uint64_t packed_sequence_bytes,
        std::uint64_t tile_count,
        std::uint32_t minimizer_word_kind,
        std::uint32_t minimizer_words32,
        std::uint64_t max_minimizers);
    void destroy_buffer(Buffer& buffer) noexcept;

    // One complete Phase A chaining submission. This records all H2D copies,
    // the compute dispatch, all D2H copies, and all barriers into one command
    // buffer lifecycle under one queue lock. Do not call separate public
    // upload/download helpers from the hot chaining path.
    void run_chaining_tiled_dp_roundtrip(
        const ChainingPipelineKey& key,
        Buffer& transfer_scratch,
        Buffer& anchors,
        Buffer& ranges,
        Buffer& best_scores,
        Buffer& predecessors,
        std::span<const std::byte> anchors_bytes,
        std::span<const std::byte> ranges_bytes,
        std::span<std::byte> best_scores_bytes,
        std::span<std::byte> predecessors_bytes,
        std::uint32_t anchor_count,
        std::uint32_t range_count,
        std::int32_t max_dist_x,
        std::int32_t max_dist_y,
        std::int32_t bandwidth,
        std::int32_t max_iter,
        float gap_penalty,
        float skip_penalty,
        std::int32_t max_chain_skip = 0);

    [[nodiscard]] GpuCoordinateChainTraceResult run_chaining_coordinate_trace_roundtrip(
        const ChainingPipelineKey& key,
        Buffer& transfer_scratch,
        Buffer& anchors,
        Buffer& ranges,
        Buffer& best_scores,
        Buffer& predecessors,
        std::span<const std::byte> anchors_bytes,
        std::span<const std::byte> ranges_bytes,
        std::span<const std::byte> best_scores_bytes,
        std::span<const std::byte> predecessors_bytes,
        std::uint32_t anchor_count,
        std::uint32_t range_count,
        std::uint64_t query_len,
        std::int32_t max_dist_x,
        std::int32_t max_dist_y,
        std::int32_t bandwidth,
        std::int32_t max_iter,
        float gap_penalty,
        float skip_penalty,
        std::int32_t max_chain_skip,
        std::int32_t min_chain_score,
        std::int32_t min_seed_count,
        std::uint32_t probe_limit);

    // DOC0307 Phase B — seed-hit lookup (lookup + bitonic sort). It records
    // every H2D copy, dispatch, barrier, and D2H copy under one queue lock,
    // mirroring the Phase A roundtrip discipline.
    Buffer run_generate_units_roundtrip(
        std::uint32_t word_bits,
        std::span<const std::uint32_t> cdf,
        GenerateUnitsPush push,
        std::uint64_t out_block_bytes);
    void download_slice(
        const Buffer& src,
        std::uint64_t src_byte_offset,
        std::span<std::byte> dst);

    SeedLookupResult run_seed_lookup_roundtrip(
        RuntimeSeedLookupInputs inputs,
        std::span<const std::uint32_t> prefix_counts,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::uint64_t exact_anchor_count,
        const SeedAnchorSortPlan& sort_plan);

    // DOC0310 Phase C — accessor for the per-runtime reusable arena (§7).
    [[nodiscard]] GpuArena& phase_c_arena();

    // DOC0310/DOC0351 Phase C — parallel ordered tile sketch (§9). The emit is
    // tile-parallel for every supported alphabet.
    RuntimeSketchResult run_phase_c_sketch_roundtrip(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_minimizers,
        std::span<const std::uint32_t> query_gate_words = {},
        std::uint32_t query_gate_mask = 0u);

    RuntimeSketchBatchResult run_phase_c_sketch_tile_batch_roundtrip(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::span<const PhaseCSketchTileWire> tiles,
        std::span<const PhaseCSketchQueryLayoutWire> query_layouts,
        std::uint64_t max_minimizers,
        VulkanSketchHostMaterialization materialization);

    // DOC0310 Phase C — build the target minimizer index entirely on device
    // (§12): exact tile count/scan → deterministic emit → device radix sort →
    // dense sorted buckets. `max_target_minimizers` is only the caller's
    // planning estimate; exact T drives checked allocation and admission.
    GpuBuiltIndex run_phase_c_build_target_index(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_target_minimizers,
        std::span<const std::uint32_t> query_gate_words = {},
        std::uint32_t query_gate_mask = 0u,
        GpuBuiltIndexGateIdentity query_gate_identity = {},
        bioseq::parallelism::MemoryPermit* peak_permit = nullptr);

    // DOC0345 Patch B — GPU per-segment sorted-pair seam: tiled `emit_mode=2`
    // emit of (kmer, encoded_pos) + a GPU two-key stable radix sort (by encoded
    // pos then kmer -> (kmer, encoded_pos) order). Segment builders still use
    // the host pair sort seam today, but DOC0348's tiled dna5/generic sketch
    // ownership issue is fixed.
    GpuSortedPairRun run_phase_c_sketch_sort_pairs_roundtrip(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_target_minimizers);

    // DOC0346 — order a batch of in-RAM compact sequences on device. Extracts a
    // 64-bit canonical lexicographic key per row, LSD-radix-sorts (reusing the
    // Phase C radix), then breaks equal-key ties with GPU full canonical compares.
    // Runs longer than LOCAL_SIZE_X trigger a device-side global bitonic compare
    // pass over the padded permutation; no host refinement is permitted here.
    [[nodiscard]] std::vector<std::uint32_t> run_sequence_sort_roundtrip(
        std::span<const std::byte> packed_units,
        std::span<const SeqDescWire> descs,
        const SeqSortKernelParams& params);
    [[nodiscard]] std::vector<std::uint32_t> run_sequence_sort_roundtrip(
        std::span<const SeqSortUploadChunk> chunks,
        std::span<const SeqDescWire> descs,
        const SeqSortKernelParams& params);

    // Score/end-point-only banded affine extension batch. Inputs are uint32
    // alphabet ranks; unsupported geometry throws `VulkanRuntimeError` before
    // dispatch, and shader-side rejection is treated as a runtime error.
    [[nodiscard]] std::vector<GpuAlignmentExtensionResultWire>
    run_alignment_extension_roundtrip(
        std::span<const std::uint32_t> query_symbols,
        std::span<const std::uint32_t> target_symbols,
        std::span<const GpuAlignmentExtensionJobWire> jobs,
        const GpuAlignmentExtensionParams& params);

    /// Rank-buffer SneakySnake lower-bound batch. Valid unequal-length and
    /// unsupported jobs are returned conservatively with an explicit status;
    /// malformed host ranges fail before dispatch.
    [[nodiscard]] std::vector<GpuSneakySnakeResultWire>
    run_sneaky_snake_rank_batch_roundtrip(
        std::span<const std::uint8_t> query_ranks,
        std::span<const std::uint8_t> target_ranks,
        std::span<const GpuSneakySnakeJobWire> jobs);

    // DOC0354 Part 2 first slice — CIGAR-capable GPU extension ABI. This
    // exact-match implementation writes packed CIGAR ops on device and rejects
    // unsupported geometry/mismatches; it does not fall back to CPU extension.
    [[nodiscard]] AlignmentCigarBatchResult
    run_alignment_simple_cigar_roundtrip(
        std::span<const std::uint32_t> query_symbols,
        std::span<const std::uint32_t> target_symbols,
        std::span<const GpuAlignmentCigarJobWire> jobs,
        const GpuAlignmentCigarParams& params,
        std::uint32_t packed_cigar_capacity);

    [[nodiscard]] std::uint64_t seq_sort_dispatch_count() const noexcept {
        return seq_sort_dispatch_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t seq_sort_global_compare_dispatch_count() const noexcept {
        return seq_sort_global_compare_dispatch_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t seq_sort_dense_scatter_dispatch_count() const noexcept {
        return seq_sort_dense_scatter_dispatch_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t seq_sort_uploaded_bytes_count() const noexcept {
        return seq_sort_uploaded_bytes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t alignment_extension_dispatch_count() const noexcept {
        return alignment_extension_dispatch_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t sneaky_snake_dispatch_count() const noexcept {
        return sneaky_snake_dispatch_count_.load(std::memory_order_relaxed);
    }

    // DOC0310 Phase C — GPU seed count + emit + device anchor sort (§14, §13).
    // Counts anchors on device (no host prepass), prefix-scans, emits anchors,
    // and stable-radix-sorts them by `Anchor::ref_bits()`. Returns the sorted
    // 24-byte anchor bytes.
    SeedLookupResult run_phase_c_seed_roundtrip(
        const PhaseCSeedInputs& inputs,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::int32_t max_occ);

    /// Select persisted partitions without materializing query hashes on the
    /// host. The device marks and compacts a target-wide ordinal union; only the
    /// resulting stable ordinals cross back to the caller.
    [[nodiscard]] GpuPersistedDirectorySelectionResult
    run_persisted_directory_ordinal_selection(
        const GpuPersistedDirectorySelectionInputs& inputs);

    [[nodiscard]] GpuGrouped32RuntimeStreamState
    begin_grouped32_runtime_stream(
        const GpuGrouped32RuntimeStateInputs& inputs);
    [[nodiscard]] GpuGrouped32RuntimeControlReceipt
    run_grouped32_control_validate_count(
        GpuGrouped32RuntimeStreamState& state,
        const GpuGrouped32RawGroupInputs& inputs);
    [[nodiscard]] GpuGrouped32RuntimeAuthorizationReceipt
    run_grouped32_count_finalize_authorize(
        GpuGrouped32RuntimeStreamState& state,
        std::uint64_t receipt_identity);
    void bind_grouped32_expected_position_groups(
        GpuGrouped32RuntimeStreamState& state,
        std::uint32_t expected_position_groups);
    [[nodiscard]] GpuGrouped32RuntimePositionReceipt
    run_grouped32_position_validate_emit(
        GpuGrouped32RuntimeStreamState& state,
        const GpuGrouped32RawGroupInputs& inputs);
    [[nodiscard]] GpuGrouped32RuntimeSealReceipt
    run_grouped32_target_seal(
        GpuGrouped32RuntimeStreamState& state,
        std::uint64_t receipt_identity);

    GpuCoordinateOnlyResultWire run_phase_c_seed_to_coordinate_roundtrip(
        const PhaseCSeedInputs& inputs,
        std::uint64_t query_len,
        std::uint64_t target_len,
        std::uint32_t kmer_size,
        std::int32_t max_occ,
        const ChainingPipelineKey& chain_key,
        std::int32_t max_dist_x,
        std::int32_t max_dist_y,
        std::int32_t bandwidth,
        std::int32_t max_iter,
        float gap_penalty,
        float skip_penalty,
        std::int32_t max_chain_skip,
        std::int32_t min_chain_score,
        std::int32_t min_seed_count,
        std::uint32_t probe_limit);

    PhaseCSeedCoordinateBatchRun run_phase_c_seed_to_coordinate_multi_query_batch_roundtrip(
        const PhaseCSeedQueryInputs& query_inputs,
        std::span<const PhaseCSeedCoordinateJob> jobs,
        std::uint32_t kmer_size,
        std::int32_t max_occ,
        PhaseCSeedCoordinateAlgorithm coordinate_algorithm,
        const ChainingPipelineKey& chain_key,
        std::int32_t max_dist_x,
        std::int32_t max_dist_y,
        std::int32_t bandwidth,
        std::int32_t max_iter,
        float gap_penalty,
        float skip_penalty,
        std::int32_t max_chain_skip,
        std::int32_t min_chain_score,
        std::int32_t min_seed_count,
        std::uint32_t probe_limit,
        DeviceQuerySketchAuthority query_authority = {});

    std::vector<GpuCoordinateOnlyResultWire> run_phase_c_seed_to_coordinate_batch_roundtrip(
        const PhaseCSeedQueryInputs& query_inputs,
        std::span<const PhaseCSeedCoordinateJob> jobs,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::int32_t max_occ,
        PhaseCSeedCoordinateAlgorithm coordinate_algorithm,
        const ChainingPipelineKey& chain_key,
        std::int32_t max_dist_x,
        std::int32_t max_dist_y,
        std::int32_t bandwidth,
        std::int32_t max_iter,
        float gap_penalty,
        float skip_penalty,
        std::int32_t max_chain_skip,
        std::int32_t min_chain_score,
        std::int32_t min_seed_count,
        std::uint32_t probe_limit);

    /// Largest power-of-two anchor count the GPU bitonic sort can handle in one
    /// workgroup on this device (bounded by workgroup invocations + shared mem).
    /// Larger anchor spans use the scalable stable GPU radix path.
    [[nodiscard]] std::uint32_t gpu_anchor_sort_capacity() const noexcept {
        return gpu_anchor_sort_capacity_;
    }

private:
    [[nodiscard]] static runtime_detail::
        PersistedShapeV5CoordinateFinalizationPlan
    prepare_persisted_shape_v5_coordinate_finalization(
        const PersistedShapeV5CoordinateOperationProof& proof,
        const PersistedShapeV5CoordinateOperationEvidencePreview&
            expected_preview,
        const VulkanFeatureSet& live_features,
        bool seed_metrics_claimed) noexcept;

    struct PipelineCacheAdmissionScope {
        Runtime* owner = nullptr;
        explicit PipelineCacheAdmissionScope(Runtime& runtime) noexcept
            : owner(&runtime) {}
        PipelineCacheAdmissionScope(const PipelineCacheAdmissionScope&) =
            delete;
        PipelineCacheAdmissionScope& operator=(
            const PipelineCacheAdmissionScope&) = delete;
        ~PipelineCacheAdmissionScope() {
            if (owner != nullptr) {
                owner->reconcile_pipeline_cache_metadata_locked();
            }
        }
    };

    struct OwnedPipelineHandles {
        VkDevice device = VK_NULL_HANDLE;
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;

        OwnedPipelineHandles() = default;
        explicit OwnedPipelineHandles(VkDevice owner) noexcept
            : device(owner) {}
        OwnedPipelineHandles(const OwnedPipelineHandles&) = delete;
        OwnedPipelineHandles& operator=(const OwnedPipelineHandles&) = delete;
        OwnedPipelineHandles(OwnedPipelineHandles&& other) noexcept
            : device(std::exchange(other.device, VK_NULL_HANDLE)),
              set_layout(std::exchange(
                  other.set_layout, VK_NULL_HANDLE)),
              pipeline_layout(std::exchange(
                  other.pipeline_layout, VK_NULL_HANDLE)),
              descriptor_pool(std::exchange(
                  other.descriptor_pool, VK_NULL_HANDLE)),
              module(std::exchange(other.module, VK_NULL_HANDLE)),
              pipeline(std::exchange(other.pipeline, VK_NULL_HANDLE)) {}
        OwnedPipelineHandles& operator=(OwnedPipelineHandles&& other) noexcept {
            if (this != &other) {
                reset();
                device = std::exchange(other.device, VK_NULL_HANDLE);
                set_layout = std::exchange(
                    other.set_layout, VK_NULL_HANDLE);
                pipeline_layout = std::exchange(
                    other.pipeline_layout, VK_NULL_HANDLE);
                descriptor_pool = std::exchange(
                    other.descriptor_pool, VK_NULL_HANDLE);
                module = std::exchange(other.module, VK_NULL_HANDLE);
                pipeline = std::exchange(other.pipeline, VK_NULL_HANDLE);
            }
            return *this;
        }
        ~OwnedPipelineHandles() { reset(); }

        void reset() noexcept;
    };

    /// One cached compute pipeline plus its shader module, keyed by tile/local.
    struct PipelineEntry {
        ChainingPipelineKey key{};
        OwnedPipelineHandles handles{};
        std::string entry_name = "main";
        std::unique_ptr<const RuntimeMetalArchivePipelineReceipt>
            metal_archive_receipt{};
        std::uint64_t pipeline_create_ns = 0u;

        PipelineEntry() = default;
        PipelineEntry(const PipelineEntry&) = delete;
        PipelineEntry& operator=(const PipelineEntry&) = delete;
        PipelineEntry(PipelineEntry&&) noexcept = default;
        PipelineEntry& operator=(PipelineEntry&&) noexcept = default;
    };

    struct ChainingPipeline {
        VkPipeline pipeline = VK_NULL_HANDLE;
        ShaderVariantKey shader_key{};
        const RuntimeMetalArchivePipelineReceipt* metal_archive_receipt =
            nullptr;
    };

    /// DOC0307 §4.5 — one cached Phase B compute pipeline with its own
    /// descriptor-set layout (variable binding count), pipeline layout (variable
    /// push-constant size), descriptor pool, and a single reusable descriptor
    /// set. Keyed by (kind, local_size, max_span) — the only fields that change
    /// the emitted SPIR-V; alphabet/word routing is a runtime push constant.
    struct PhaseBPipeline {
        ShaderKind kind{};
        std::uint32_t local_size = 0;
        std::uint32_t max_span = 0;
        std::uint32_t binding_count = 0;
        std::uint32_t push_size = 0;
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const RuntimeMetalArchivePipelineReceipt* metal_archive_receipt =
            nullptr;
    };

    struct PhaseBPipelineOwner {
        ShaderKind kind{};
        std::uint32_t local_size = 0;
        std::uint32_t max_span = 0;
        std::uint32_t binding_count = 0;
        std::uint32_t push_size = 0;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        OwnedPipelineHandles handles{};
        std::string entry_name = "main";
        std::unique_ptr<const RuntimeMetalArchivePipelineReceipt>
            metal_archive_receipt{};
        std::uint64_t pipeline_create_ns = 0u;

        PhaseBPipelineOwner() = default;
        PhaseBPipelineOwner(const PhaseBPipelineOwner&) = delete;
        PhaseBPipelineOwner& operator=(const PhaseBPipelineOwner&) = delete;
        PhaseBPipelineOwner(PhaseBPipelineOwner&&) noexcept = default;
        PhaseBPipelineOwner& operator=(PhaseBPipelineOwner&&) noexcept =
            default;

        [[nodiscard]] PhaseBPipeline view() const noexcept {
            return PhaseBPipeline{
                .kind = kind,
                .local_size = local_size,
                .max_span = max_span,
                .binding_count = binding_count,
                .push_size = push_size,
                .set_layout = handles.set_layout,
                .pipeline_layout = handles.pipeline_layout,
                .descriptor_pool = handles.descriptor_pool,
                .descriptor_set = descriptor_set,
                .module = handles.module,
                .pipeline = handles.pipeline,
                .metal_archive_receipt = metal_archive_receipt.get(),
            };
        }
    };

    // Returns a POD non-owning view. Canonical move-only owners remain in the
    // cache and transfer Vulkan handles exactly once across reallocation.
    PhaseBPipeline get_phase_b_pipeline_locked(
        ShaderKind kind, std::uint32_t local_size, std::uint32_t max_span);
    void bind_phase_b_descriptors_locked(
        const PhaseBPipeline& entry,
        std::span<const VkBuffer> buffers);
    // DOC0310 Phase C — bind storage descriptors as explicit (buffer, offset,
    // range) so arena-backed slices that share a physical buffer bind correctly.
    void bind_descriptors_offsets_locked(
        const PhaseBPipeline& entry,
        std::span<const VkDescriptorBufferInfo> infos);

    // DOC0310 Phase C — fresh-descriptor-set-per-dispatch pool. Lets one command
    // buffer record many dispatches of the same pipeline with different buffer
    // bindings (ping-pong sort passes) without re-updating a single set in
    // flight. Reset once per frontend op.
    void ensure_phase_c_pool_locked();
    void reset_phase_c_pool_locked();
    [[nodiscard]] VkDescriptorSet alloc_phase_c_set_locked(VkDescriptorSetLayout layout);
    void record_phase_c_dispatch_locked(
        VkCommandBuffer cmd, const PhaseBPipeline& pipe,
        std::span<const VkDescriptorBufferInfo> infos,
        const void* push, std::uint32_t push_size, std::uint32_t group_count,
        RuntimePipelineUseCollector* collector = nullptr,
        std::size_t collector_job_index = 0u);
    // Record one full device radix sort (all passes) into an open command
    // buffer. Sorts the key/value arena slices in place via ping-pong scratch.
    void record_radix_sort_locked(
        VkCommandBuffer cmd,
        const GpuArenaSlice& keys, const GpuArenaSlice& values,
        const GpuArenaSlice& keys_scratch, const GpuArenaSlice& values_scratch,
        const GpuArenaSlice& histogram,
        const GpuSortPlan& plan, std::uint32_t num_workgroups,
        std::uint32_t elems_per_wg, std::uint32_t value_words,
        bool& result_in_scratch,
        RuntimePipelineUseCollector* collector = nullptr,
        std::size_t collector_job_index = 0u);
    // DOC0345 Patch B — two-key stable sort: radix by value (encoded_pos) then
    // radix by key (kmer). Because the second sort is stable the result is
    // (kmer, encoded_pos) ordered.
    void record_stable_pair_sort_by_value_then_key_locked(
        VkCommandBuffer command_buffer,
        SortedPairDeviceViews& views,
        GpuArenaSlice hist,
        GpuSortPlan value_sort_plan,
        GpuSortPlan key_sort_plan,
        std::uint32_t num_workgroups,
        std::uint32_t elems_per_workgroup);

    void init();
    void shutdown() noexcept;
    void resolve_moltenvk_private_api() noexcept;
    [[nodiscard]] bool use_shader_provider_locked() const noexcept;
    std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    [[nodiscard]] std::uint64_t buffer_memory_requirement_locked(
        std::uint64_t bytes, VkBufferUsageFlags usage) const;
    [[nodiscard]] std::uint64_t pipeline_cache_metadata_bytes_locked()
        const noexcept;
    void admit_pipeline_cache_growth_locked(
        std::size_t normal_capacity,
        std::size_t phase_b_capacity);
    void reconcile_pipeline_cache_metadata_locked() noexcept;
    ChainingPipeline create_pipeline_locked(const ChainingPipelineKey& key);
    void upload_locked(Buffer& transfer_scratch, Buffer& dst, std::span<const std::byte> bytes,
                       VkAccessFlags dst_access);
    void download_locked(Buffer& transfer_scratch, const Buffer& src, std::span<std::byte> bytes);
    [[nodiscard]] std::uint64_t next_operation_proof_identity_locked() {
        constexpr std::uint64_t exhausted =
            (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t identity =
            operation_proof_identity_.load(std::memory_order_relaxed);
        while (identity != exhausted) {
            if (operation_proof_identity_.compare_exchange_weak(
                    identity,
                    identity + 1u,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return identity;
            }
        }
        throw VulkanRuntimeError(
            "Vulkan operation proof identity space exhausted");
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    // Declared before both caches so cache-owned strings/receipts/entries are
    // destroyed before their aggregate metadata reservation is returned.
    bioseq::parallelism::MemoryPermit pipeline_cache_metadata_permit_;
    std::vector<PipelineEntry> pipeline_cache_;
    std::vector<PhaseBPipelineOwner> phase_b_pipelines_;
    bool seed_metrics_claimed_ = false;
    std::uint32_t gpu_anchor_sort_capacity_ = 0;
    std::optional<GpuArena> phase_c_arena_;  // DOC0310 §7 — lazily created.
    VkDescriptorPool phase_c_pool_ = VK_NULL_HANDLE;  // DOC0310 — fresh-set pool.
    using PFN_vkSetWorkgroupSizeMVK_Bioseq =
        void (VKAPI_PTR*)(VkShaderModule module, std::uint32_t x, std::uint32_t y, std::uint32_t z);
    PFN_vkSetWorkgroupSizeMVK_Bioseq set_workgroup_size_mvk_ = nullptr;
    bool metallib_provider_capable_ = false;

    VulkanFeatureSet features_{};
    std::string init_error_{};
    std::atomic<std::uint64_t> chaining_dispatch_count_{0};
    std::atomic<std::uint64_t> generate_units_dispatch_count_{0};
    std::atomic<std::uint64_t> seq_sort_dispatch_count_{0};  // DOC0346
    std::atomic<std::uint64_t> seq_sort_global_compare_dispatch_count_{0};  // DOC0360
    std::atomic<std::uint64_t> seq_sort_dense_scatter_dispatch_count_{0};  // DOC0360
    std::atomic<std::uint64_t> seq_sort_uploaded_bytes_{0};  // DOC0360 full-payload proof
    std::atomic<std::uint64_t> alignment_extension_dispatch_count_{0};
    std::atomic<std::uint64_t> sneaky_snake_dispatch_count_{0};
    std::atomic<std::uint64_t> grouped32_stream_identity_{1};
    std::atomic<std::uint64_t> operation_proof_identity_{1};
    std::mutex mu_;
};

[[nodiscard]] inline bool hardware_device_available(const Runtime& runtime) noexcept {
    return runtime.usable() && !runtime.features().software_device;
}

inline void require_hardware_device(const Runtime& runtime, const char* operation) {
    if (!runtime.usable()) {
        throw VulkanRuntimeError(fmt::format(
            "{} selected but Vulkan runtime is unavailable: {}",
            operation,
            runtime.initialization_error()));
    }
    if (runtime.features().software_device) {
        throw VulkanRuntimeError(fmt::format(
            "{} selected but active Vulkan device '{}' is a CPU/software device "
            "(VkPhysicalDeviceType={}); explicit GPU execution requires a hardware Vulkan device",
            operation,
            runtime.features().device_name.data(),
            runtime.features().device_type));
    }
}

[[nodiscard]] inline std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) noexcept {
    constexpr std::uint64_t max_value = (std::numeric_limits<std::uint64_t>::max)();
    if (a > max_value - b) return max_value;
    return a + b;
}

[[nodiscard]] inline std::uint64_t sat_max(std::uint64_t a, std::uint64_t b) noexcept {
    return a > b ? a : b;
}

[[nodiscard]] inline std::uint64_t phase_c_seed_exact_reserved_bytes(
    std::uint64_t anchor_count,
    std::uint64_t workgroup_count) noexcept
{
    using bioseq::parallelism::saturating_mul;
    const std::uint64_t anchors = saturating_mul(anchor_count, 24u);
    const std::uint64_t keys = saturating_mul(anchor_count, 8u);
    const std::uint64_t anchor_scratch = anchors;
    const std::uint64_t key_scratch = keys;
    const std::uint64_t histogram =
        saturating_mul(saturating_mul(workgroup_count, 256u), 4u);
    const std::uint64_t host_download = anchors;
    std::uint64_t total = 0;
    total = sat_add(total, anchors);
    total = sat_add(total, keys);
    total = sat_add(total, anchor_scratch);
    total = sat_add(total, key_scratch);
    total = sat_add(total, histogram);
    total = sat_add(total, host_download);
    return total;
}

}  // namespace bioseq::gpu::vulkan

// ============================================================================
// Implementation
// ============================================================================

namespace bioseq::gpu::vulkan {

namespace {

/// Throw `VulkanRuntimeError` on any non-success VkResult. A free function
/// rather than a capture-all lambda so it leaves no per-call closure in the hot
/// path and is trivially inlinable.
inline void check_vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw VulkanRuntimeError(
            fmt::format("{}: VkResult {}", what, static_cast<int>(result)));
    }
}

#define BIOSEQ_VULKAN_RUNTIME_FUNCTIONS(X) \
    X(vkAllocateCommandBuffers) \
    X(vkAllocateDescriptorSets) \
    X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) \
    X(vkBindBufferMemory) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindPipeline) \
    X(vkCmdCopyBuffer) \
    X(vkCmdDispatch) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdPushConstants) \
    X(vkCreateBuffer) \
    X(vkCreateCommandPool) \
    X(vkCreateComputePipelines) \
    X(vkCreateDescriptorPool) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreateDevice) \
    X(vkCreateFence) \
    X(vkCreateInstance) \
    X(vkCreatePipelineLayout) \
    X(vkCreateShaderModule) \
    X(vkDestroyBuffer) \
    X(vkDestroyCommandPool) \
    X(vkDestroyDescriptorPool) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyDevice) \
    X(vkDestroyFence) \
    X(vkDestroyInstance) \
    X(vkDestroyPipeline) \
    X(vkDestroyPipelineLayout) \
    X(vkDestroyShaderModule) \
    X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumeratePhysicalDevices) \
    X(vkFreeMemory) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetDeviceQueue) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkMapMemory) \
    X(vkQueueSubmit) \
    X(vkResetCommandBuffer) \
    X(vkResetDescriptorPool) \
    X(vkResetFences) \
    X(vkUnmapMemory) \
    X(vkUpdateDescriptorSets) \
    X(vkWaitForFences)

#define BIOSEQ_DECLARE_VULKAN_FUNCTION(name) inline PFN_##name name = nullptr;
BIOSEQ_VULKAN_RUNTIME_FUNCTIONS(BIOSEQ_DECLARE_VULKAN_FUNCTION)
#undef BIOSEQ_DECLARE_VULKAN_FUNCTION

constexpr const char* kPortabilityEnumeration = "VK_KHR_portability_enumeration";
constexpr const char* kGetPhysicalDeviceProperties2 = "VK_KHR_get_physical_device_properties2";
constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";
constexpr const char* kLayerSettings = VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;

/// True if `name` appears in a list of VkExtensionProperties.
[[nodiscard]] bool has_extension(
    const std::vector<VkExtensionProperties>& available, const char* name) noexcept {
    for (const auto& ext : available) {
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    }
    return false;
}

[[nodiscard]] std::vector<VkExtensionProperties> instance_extensions() {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data()) != VK_SUCCESS) {
        return {};
    }
    return props;
}

[[nodiscard]] std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data()) != VK_SUCCESS) {
        return {};
    }
    return props;
}

/// The vcpkg Vulkan loader used by the bench/test binaries does not search
/// Homebrew's `/opt/homebrew/etc/vulkan/icd.d` on macOS. If the user has not
/// provided an explicit ICD override, install the Homebrew MoltenVK ICD path
/// before the first loader query so `vkCreateInstance` sees the portability ICD.
void ensure_homebrew_moltenvk_icd_visible() {
#if defined(__APPLE__)
    (void)::setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);
    if (std::getenv("VK_ICD_FILENAMES") != nullptr) {
        return;
    }
    constexpr const char* candidates[] = {
        "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json",
    };
    for (const char* path : candidates) {
        if (std::FILE* f = std::fopen(path, "r")) {
            std::fclose(f);
            if (::setenv("VK_ICD_FILENAMES", path, 0) == 0) {
                bioseq_log<DBG>("[vulkan_runtime] using MoltenVK ICD manifest {}", path);
            }
            return;
        }
    }
#endif
}

struct RuntimeMetalArchiveConfigurationSlot {
    std::mutex mu;
    std::shared_ptr<const RuntimeMetalArchiveConfigurationImage> configuration;
    bool sealed = false;
    std::atomic<bool> installed_accepted{false};
    std::atomic<std::uint64_t> install_validation_count{0u};
    std::atomic<std::uint64_t> live_gate_calls{0u};
    std::atomic<std::uint64_t> live_configuration_scan_count{0u};
};

RuntimeMetalArchiveConfigurationSlot&
runtime_metal_archive_configuration_slot() noexcept {
    static RuntimeMetalArchiveConfigurationSlot slot;
    return slot;
}

[[nodiscard]] std::shared_ptr<const RuntimeMetalArchiveConfigurationImage>
runtime_metal_archive_configuration_snapshot() {
    auto& slot = runtime_metal_archive_configuration_slot();
    std::lock_guard<std::mutex> guard(slot.mu);
    return slot.configuration;
}

}  // namespace

void Runtime::OwnedPipelineHandles::reset() noexcept {
    if (device != VK_NULL_HANDLE) {
        if (pipeline != VK_NULL_HANDLE && vkDestroyPipeline != nullptr) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (module != VK_NULL_HANDLE && vkDestroyShaderModule != nullptr) {
            vkDestroyShaderModule(device, module, nullptr);
        }
        if (descriptor_pool != VK_NULL_HANDLE
            && vkDestroyDescriptorPool != nullptr) {
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE
            && vkDestroyPipelineLayout != nullptr) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        if (set_layout != VK_NULL_HANDLE
            && vkDestroyDescriptorSetLayout != nullptr) {
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        }
    }
    device = VK_NULL_HANDLE;
    set_layout = VK_NULL_HANDLE;
    pipeline_layout = VK_NULL_HANDLE;
    descriptor_pool = VK_NULL_HANDLE;
    module = VK_NULL_HANDLE;
    pipeline = VK_NULL_HANDLE;
}

std::expected<std::shared_ptr<const RuntimeMetalArchiveConfigurationImage>,
              std::string>
make_runtime_metal_archive_configuration_image(
    std::shared_ptr<const SealedShaderCacheSeedImage> seed_image,
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid,
    std::uint64_t metal_device_registry_id) {
    if (!seed_image || !seed_image->accepted()) {
        return std::unexpected(
            "runtime Metal archive configuration has no accepted seed image");
    }
    std::uint64_t archive_count = 0u;
    for (const SealedShaderCacheArtifactImage& artifact :
         seed_image->artifacts()) {
        archive_count += artifact.descriptor.kind
                == SealedShaderCacheArtifactKind::metal_binary_archive
            ? 1u : 0u;
    }
    if (archive_count == 0u
        || archive_count
            > sealed_shader_cache_detail::maximum_seed_image_artifacts) {
        return std::unexpected(
            "runtime Metal archive configuration archive count is invalid");
    }
    using bioseq::parallelism::saturating_add;
    using bioseq::parallelism::saturating_mul;
    std::uint64_t retained = sizeof(RuntimeMetalArchiveConfigurationImage);
    retained = saturating_add(
        saturating_add(
            retained,
            saturating_mul(
                archive_count,
                sizeof(RuntimeMetalArchiveSpec)
                    + 2u * (sealed_shader_cache_detail::
                                maximum_manifest_filename_bytes
                            + 1024u))),
        2u * 1024u);
    const std::uint64_t peak = saturating_mul(
        retained, bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    if (retained == (std::numeric_limits<std::uint64_t>::max)()
        || peak == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(
            "runtime Metal archive configuration admission saturated");
    }
    bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(peak, &admission_failure);
    if (!permit) {
        return std::unexpected(shader_cache_message(
            "runtime Metal archive configuration admission denied before allocation: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(
                admission_failure.reason),
            " requested=", admission_failure.requested_bytes));
    }
    try {
        RuntimeMetalArchiveConfiguration configuration{};
        configuration.seed_image = std::move(seed_image);
        configuration.vulkan_device_uuid = vulkan_device_uuid;
        configuration.metal_device_registry_id = metal_device_registry_id;
        configuration.archives.reserve(
            static_cast<std::size_t>(archive_count));
        std::optional<MetalArchiveNegativeErrorFingerprint> common_error;
        for (std::size_t ordinal = 0u;
             ordinal < configuration.seed_image->artifacts().size();
             ++ordinal) {
            const SealedShaderCacheArtifactImage& artifact =
                configuration.seed_image->artifacts()[ordinal];
            if (artifact.descriptor.kind
                != SealedShaderCacheArtifactKind::metal_binary_archive) {
                continue;
            }
            const std::string qualification_filename =
                artifact.descriptor.filename + ".qualification";
            const auto* qualification_image =
                sealed_shader_cache_detail::find_artifact_image(
                    configuration.seed_image->artifacts(),
                    SealedShaderCacheArtifactKind::
                        metal_binary_archive_qualification,
                    artifact.descriptor.key_index, qualification_filename);
            if (qualification_image == nullptr) {
                return std::unexpected(shader_cache_message(
                    "runtime Metal archive qualification missing for ",
                    artifact.descriptor.filename));
            }
            auto qualification = sealed_shader_cache_detail::
                parse_metal_archive_qualification_receipt(
                    qualification_image->bytes);
            if (!qualification
                || !qualification->production_accepted()
                || qualification->archive_size != artifact.descriptor.size
                || qualification->archive_sha256
                    != artifact.descriptor.sha256) {
                return std::unexpected(shader_cache_message(
                    "runtime Metal archive qualification mismatch for ",
                    artifact.descriptor.filename));
            }
            if (common_error
                && *common_error != qualification->negative_error) {
                return std::unexpected(
                    "runtime Metal archive qualifications disagree on NSError fingerprint");
            }
            common_error = qualification->negative_error;
            if (configuration.qualification_receipt_count == 0u) {
                configuration.qualification_version =
                    qualification->wire_version;
                configuration.helper_protocol_version =
                    qualification->helper_protocol_version;
                configuration.helper_build_identity =
                    qualification->helper_build_identity;
                configuration.helper_source = qualification->helper_source;
                configuration.helper_executable_size =
                    qualification->helper_executable_size;
                configuration.helper_executable_sha256 =
                    qualification->helper_executable_sha256;
                configuration.helper_metal_registry_id =
                    qualification->helper_metal_registry_id;
            } else if (configuration.qualification_version
                           != qualification->wire_version
                || configuration.helper_protocol_version
                    != qualification->helper_protocol_version
                || configuration.helper_build_identity
                    != qualification->helper_build_identity
                || configuration.helper_source != qualification->helper_source
                || configuration.helper_executable_size
                    != qualification->helper_executable_size
                || configuration.helper_executable_sha256
                    != qualification->helper_executable_sha256
                || configuration.helper_metal_registry_id
                    != qualification->helper_metal_registry_id) {
                return std::unexpected(
                    "runtime Metal archive qualifications disagree on helper provenance");
            }
            ++configuration.qualification_receipt_count;
            configuration.archives.push_back(RuntimeMetalArchiveSpec{
                .artifact_ordinal = static_cast<std::uint32_t>(ordinal),
                .artifact_filename = artifact.descriptor.filename,
                .size = artifact.descriptor.size,
                .sha256 = artifact.descriptor.sha256,
                .negative_error_domain =
                    std::move(qualification->negative_error.domain),
                .negative_error_code = qualification->negative_error.code,
            });
        }
        if (!configuration.accepted()) {
            return std::unexpected(
                "runtime Metal archive configuration factory produced invalid evidence");
        }
        auto installed = std::shared_ptr<RuntimeMetalArchiveConfigurationImage>(
            new RuntimeMetalArchiveConfigurationImage(
                std::move(configuration), peak, std::move(*permit)));
        if (!installed->reconcile() || !installed->accepted()) {
            return std::unexpected(
                "runtime Metal archive configuration admission reconciliation failed");
        }
        return installed;
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            "runtime Metal archive configuration allocation failed");
    }
}

std::expected<void, std::string> install_runtime_metal_archive_configuration(
    std::shared_ptr<const RuntimeMetalArchiveConfigurationImage> configuration) {
    if (!configuration || !configuration->accepted()) {
        return std::unexpected(
            "runtime Metal archive configuration is incomplete or non-canonical");
    }
    auto& slot = runtime_metal_archive_configuration_slot();
    slot.install_validation_count.fetch_add(1u, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(slot.mu);
    if (slot.sealed) {
        if (slot.configuration
            && slot.configuration->configuration()
                == configuration->configuration()) {
            return {};
        }
        return std::unexpected(
            "runtime Metal archive configuration is already sealed to different evidence");
    }
    slot.configuration = std::move(configuration);
    slot.sealed = true;
    slot.installed_accepted.store(true, std::memory_order_release);
    return {};
}

bool runtime_metal_archive_configuration_installed() noexcept {
    auto& slot = runtime_metal_archive_configuration_slot();
    return slot.installed_accepted.load(std::memory_order_acquire);
}

[[nodiscard]] RuntimeMetalArchiveLiveGateEvidence
runtime_metal_archive_live_gate_evidence() noexcept {
    auto& slot = runtime_metal_archive_configuration_slot();
    return RuntimeMetalArchiveLiveGateEvidence{
        .install_validation_count = slot.install_validation_count.load(
            std::memory_order_relaxed),
        .live_gate_calls = slot.live_gate_calls.load(
            std::memory_order_relaxed),
        .live_configuration_scan_count =
            slot.live_configuration_scan_count.load(
                std::memory_order_relaxed),
        .installed_accepted = slot.installed_accepted.load(
            std::memory_order_acquire),
    };
}

namespace {

#if defined(__APPLE__)
struct ObjcRuntimeFns {
    void* msg_send = nullptr;
    void* (*get_class)(const char*) = nullptr;
    void* (*sel_register_name)(const char*) = nullptr;
    void* (*object_get_class)(void*) = nullptr;
    void* (*class_get_superclass)(void*) = nullptr;
    void* (*class_get_instance_method)(void*, void*) = nullptr;
    void* (*method_get_implementation)(void*) = nullptr;
    void* (*method_set_implementation)(void*, void*) = nullptr;
};

struct MoltenVkCommandLibraryInterpose {
    ObjcRuntimeFns objc{};
    std::vector<std::byte> metallib;
    void* original_new_library_with_source = nullptr;
    void* original_new_library_with_source_async = nullptr;
    void* sel_utf8_string = nullptr;
    void* sel_new_library_with_data_error = nullptr;
    void* sel_new_library_with_source = nullptr;
    void* sel_new_library_with_source_async = nullptr;
    std::uint64_t loader_registry_id = 0u;
    std::uint64_t cached_methods_present = 0u;
    std::uint64_t cached_wrapper_methods = 0u;
    bool cached_binding_validated = false;
    bool installed = false;
    bool immutable_fail_closed = false;
    std::atomic<bool> sticky_failure{false};
    std::atomic<std::uint64_t> recognized_source_requests{0u};
    std::atomic<std::uint64_t> source_fallback_calls{0u};
};

MoltenVkCommandLibraryInterpose& moltenvk_command_interpose_state() noexcept {
    static MoltenVkCommandLibraryInterpose state;
    return state;
}

template <class Fn>
[[nodiscard]] Fn objc_msg_cast(void* fn) noexcept {
    return reinterpret_cast<Fn>(fn);
}

class ScopedObjcRelease {
public:
    ScopedObjcRelease() = default;
    ScopedObjcRelease(ObjcRuntimeFns* objc, void* release_selector,
                      void* object) noexcept
        : objc_(objc), release_selector_(release_selector), object_(object) {}
    ScopedObjcRelease(const ScopedObjcRelease&) = delete;
    ScopedObjcRelease& operator=(const ScopedObjcRelease&) = delete;
    ScopedObjcRelease(ScopedObjcRelease&& other) noexcept
        : objc_(std::exchange(other.objc_, nullptr)),
          release_selector_(std::exchange(other.release_selector_, nullptr)),
          object_(std::exchange(other.object_, nullptr)) {}
    ScopedObjcRelease& operator=(ScopedObjcRelease&&) = delete;
    ~ScopedObjcRelease() { reset(); }

    [[nodiscard]] void* get() const noexcept { return object_; }
    [[nodiscard]] void* release() noexcept {
        return std::exchange(object_, nullptr);
    }
    void reset() noexcept {
        if (objc_ != nullptr && objc_->msg_send != nullptr
            && release_selector_ != nullptr && object_ != nullptr) {
            using ReleaseFn = void (*)(void*, void*);
            objc_msg_cast<ReleaseFn>(objc_->msg_send)(
                object_, release_selector_);
        }
        object_ = nullptr;
    }

private:
    ObjcRuntimeFns* objc_ = nullptr;
    void* release_selector_ = nullptr;
    void* object_ = nullptr;
};

extern "C" void* bioseq_moltenvk_new_library_with_source(
    void* self, void* cmd, void* source, void* options, void** error)
{
    auto& state = moltenvk_command_interpose_state();
    using Utf8Fn = const char* (*)(void*, void*);
    const char* text = nullptr;
    if (source != nullptr && state.sel_utf8_string != nullptr) {
        text = objc_msg_cast<Utf8Fn>(state.objc.msg_send)(source, state.sel_utf8_string);
    }
    const bool is_moltenvk_command_source =
        text != nullptr
        && std::strstr(text, "cmdCopyBufferBytes") != nullptr
        && std::strstr(text, "vtxCmdBlitImage") != nullptr;
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] newLibraryWithSource intercepted: text=%d len=%zu is_cmd_source=%d metallib_empty=%d\n",
                 text != nullptr ? 1 : 0,
                 text != nullptr ? std::strlen(text) : 0u,
                 is_moltenvk_command_source ? 1 : 0,
                 state.metallib.empty() ? 1 : 0);
    if (state.immutable_fail_closed) {
        state.recognized_source_requests.fetch_add(
            1u, std::memory_order_relaxed);
        state.sticky_failure.store(true, std::memory_order_release);
        std::fprintf(
            stderr,
            "[bioseq-gpu-diag] cmd-interpose: immutable mode rejects every unsealed Metal source-library request\n");
        return nullptr;
    }
    if (is_moltenvk_command_source && !state.metallib.empty()) {
        using LibraryDataFn = void* (*)(void*, void*, void*, void**);
        dispatch_data_t mtl_data = dispatch_data_create(
            state.metallib.data(),
            state.metallib.size(),
            nullptr,
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (mtl_data != nullptr) {
            if (void* library = objc_msg_cast<LibraryDataFn>(state.objc.msg_send)(
                    self,
                    state.sel_new_library_with_data_error,
                    static_cast<void*>(mtl_data),
                    error);
                library != nullptr) {
                dispatch_release(mtl_data);
                std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: returned cached command library (newLibraryWithData ok)\n");
                return library;
            }
            dispatch_release(mtl_data);
            std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: newLibraryWithData FAILED, falling through to source compile\n");
        }
    }

    std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: FALL-THROUGH to real newLibraryWithSource (XPC compile)\n");
    if (is_moltenvk_command_source) {
        state.source_fallback_calls.fetch_add(1u, std::memory_order_relaxed);
    }
    using SourceFn = void* (*)(void*, void*, void*, void*, void**);
    return objc_msg_cast<SourceFn>(state.original_new_library_with_source)(
        self, cmd, source, options, error);
}

struct ObjcBlockLiteral {
    void* isa = nullptr;
    int flags = 0;
    int reserved = 0;
    void* invoke = nullptr;
    void* descriptor = nullptr;
};

void invoke_metal_library_completion(void* completion, void* library, void* error) noexcept {
    if (completion == nullptr) {
        return;
    }
    auto* block = static_cast<ObjcBlockLiteral*>(completion);
    if (block->invoke == nullptr) {
        return;
    }
    using InvokeFn = void (*)(void*, void*, void*);
    objc_msg_cast<InvokeFn>(block->invoke)(completion, library, error);
}

void invoke_metal_pipeline_completion(
    void* completion,
    void* pipeline,
    void* reflection,
    void* error) noexcept {
    if (completion == nullptr) return;
    auto* block = static_cast<ObjcBlockLiteral*>(completion);
    if (block->invoke == nullptr) return;
    using InvokeFn = void (*)(void*, void*, void*, void*);
    objc_msg_cast<InvokeFn>(block->invoke)(
        completion, pipeline, reflection, error);
}

extern "C" void bioseq_moltenvk_new_library_with_source_async(
    void* self, void* cmd, void* source, void* options, void* completion)
{
    auto& state = moltenvk_command_interpose_state();
    using Utf8Fn = const char* (*)(void*, void*);
    const char* text = nullptr;
    if (source != nullptr && state.sel_utf8_string != nullptr) {
        text = objc_msg_cast<Utf8Fn>(state.objc.msg_send)(source, state.sel_utf8_string);
    }
    const bool is_moltenvk_command_source =
        text != nullptr
        && std::strstr(text, "cmdCopyBufferBytes") != nullptr
        && std::strstr(text, "vtxCmdBlitImage") != nullptr;
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] newLibraryWithSource async intercepted: text=%d len=%zu is_cmd_source=%d metallib_empty=%d\n",
                 text != nullptr ? 1 : 0,
                 text != nullptr ? std::strlen(text) : 0u,
                 is_moltenvk_command_source ? 1 : 0,
                 state.metallib.empty() ? 1 : 0);
    if (state.immutable_fail_closed) {
        state.recognized_source_requests.fetch_add(
            1u, std::memory_order_relaxed);
        state.sticky_failure.store(true, std::memory_order_release);
        std::fprintf(
            stderr,
            "[bioseq-gpu-diag] cmd-interpose: immutable mode rejects every unsealed Metal source-library request async\n");
        invoke_metal_library_completion(completion, nullptr, nullptr);
        return;
    }
    if (is_moltenvk_command_source && !state.metallib.empty()) {
        using LibraryDataFn = void* (*)(void*, void*, void*, void**);
        dispatch_data_t mtl_data = dispatch_data_create(
            state.metallib.data(),
            state.metallib.size(),
            nullptr,
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (mtl_data != nullptr) {
            void* error = nullptr;
            void* library = objc_msg_cast<LibraryDataFn>(state.objc.msg_send)(
                self,
                state.sel_new_library_with_data_error,
                static_cast<void*>(mtl_data),
                &error);
            dispatch_release(mtl_data);
            if (library != nullptr) {
                std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: returned cached command library async (newLibraryWithData ok)\n");
                invoke_metal_library_completion(completion, library, nullptr);
                return;
            }
            std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: async newLibraryWithData FAILED, falling through to source compile\n");
        }
    }

    std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: async FALL-THROUGH to real newLibraryWithSource (XPC compile)\n");
    if (is_moltenvk_command_source) {
        state.source_fallback_calls.fetch_add(1u, std::memory_order_relaxed);
    }
    if (state.original_new_library_with_source_async == nullptr) {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: async original IMP missing; completing with null library\n");
        invoke_metal_library_completion(completion, nullptr, nullptr);
        return;
    }
    using SourceAsyncFn = void (*)(void*, void*, void*, void*, void*);
    objc_msg_cast<SourceAsyncFn>(state.original_new_library_with_source_async)(
        self, cmd, source, options, completion);
}

[[nodiscard]] bool read_file_bytes(
    const char* path, std::vector<std::byte>* out)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size <= 0) {
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(out->data()), size);
    return static_cast<bool>(input);
}

void install_moltenvk_command_library_interpose() noexcept {
    auto& state = moltenvk_command_interpose_state();
    void* installed_method = nullptr;
    void* installed_method_async = nullptr;
    const auto rollback_swizzles = [&state, &installed_method,
                                     &installed_method_async]() noexcept {
        if (state.objc.method_set_implementation != nullptr) {
            if (installed_method != nullptr
                && state.original_new_library_with_source != nullptr) {
                (void)state.objc.method_set_implementation(
                    installed_method,
                    state.original_new_library_with_source);
            }
            if (installed_method_async != nullptr
                && state.original_new_library_with_source_async != nullptr) {
                (void)state.objc.method_set_implementation(
                    installed_method_async,
                    state.original_new_library_with_source_async);
            }
        }
        state.installed = false;
        state.cached_binding_validated = false;
        state.cached_methods_present = 0u;
        state.cached_wrapper_methods = 0u;
        state.loader_registry_id = 0u;
        state.original_new_library_with_source = nullptr;
        state.original_new_library_with_source_async = nullptr;
        state.sel_utf8_string = nullptr;
        state.sel_new_library_with_data_error = nullptr;
        state.sel_new_library_with_source = nullptr;
        state.sel_new_library_with_source_async = nullptr;
        std::vector<std::byte>{}.swap(state.metallib);
        state.objc = {};
    };
    try {
    const bool immutable_fail_closed =
        runtime_metal_archive_configuration_installed();
    const bool previously_immutable_fail_closed =
        state.immutable_fail_closed;
    const auto reject_immutable_install = [
        &state, immutable_fail_closed]() noexcept {
        if (immutable_fail_closed) {
            state.sticky_failure.store(true, std::memory_order_release);
        }
    };
    if (state.installed) {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: already installed\n");
        if (immutable_fail_closed && !previously_immutable_fail_closed) {
            state.sticky_failure.store(true, std::memory_order_release);
        }
        return;
    }
    state.immutable_fail_closed = immutable_fail_closed;
    const char* path = std::getenv("BIOSEQ_MOLTENVK_COMMAND_METALLIB");
    std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: BIOSEQ_MOLTENVK_COMMAND_METALLIB=%s\n",
                 path != nullptr ? path : "(null)");
    if (!immutable_fail_closed && (path == nullptr || *path == '\0')) {
        return;
    }
    std::vector<std::byte> metallib;
    if (!immutable_fail_closed && !read_file_bytes(path, &metallib)) {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: read_file_bytes FAILED for %s\n", path);
        bioseq_log<DBG>("[vulkan_runtime] MoltenVK command metallib unavailable at {}", path);
        return;
    }
    if (immutable_fail_closed) {
        std::fprintf(
            stderr,
            "[bioseq-gpu-diag] cmd-interpose: immutable mode ignores unsealed placeholder command metallib and fails closed on recognized source\n");
    } else {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: read command metallib ok bytes=%zu\n",
                     metallib.size());
    }

    void* objc_handle = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
    void* metal_handle =
        ::dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
    if (objc_handle == nullptr || metal_handle == nullptr) {
        reject_immutable_install();
        return;
    }

    ObjcRuntimeFns objc{};
    objc.msg_send = ::dlsym(objc_handle, "objc_msgSend");
    objc.get_class = reinterpret_cast<decltype(objc.get_class)>(
        ::dlsym(objc_handle, "objc_getClass"));
    objc.sel_register_name = reinterpret_cast<decltype(objc.sel_register_name)>(
        ::dlsym(objc_handle, "sel_registerName"));
    objc.object_get_class = reinterpret_cast<decltype(objc.object_get_class)>(
        ::dlsym(objc_handle, "object_getClass"));
    objc.class_get_superclass = reinterpret_cast<decltype(objc.class_get_superclass)>(
        ::dlsym(objc_handle, "class_getSuperclass"));
    objc.class_get_instance_method = reinterpret_cast<decltype(objc.class_get_instance_method)>(
        ::dlsym(objc_handle, "class_getInstanceMethod"));
    objc.method_get_implementation = reinterpret_cast<decltype(objc.method_get_implementation)>(
        ::dlsym(objc_handle, "method_getImplementation"));
    objc.method_set_implementation = reinterpret_cast<decltype(objc.method_set_implementation)>(
        ::dlsym(objc_handle, "method_setImplementation"));
    if (objc.msg_send == nullptr || objc.get_class == nullptr
        || objc.sel_register_name == nullptr || objc.object_get_class == nullptr
        || objc.class_get_superclass == nullptr || objc.class_get_instance_method == nullptr
        || objc.method_get_implementation == nullptr
        || objc.method_set_implementation == nullptr) {
        reject_immutable_install();
        return;
    }

    using CreateDeviceFn = void* (*)();
    auto create_device = reinterpret_cast<CreateDeviceFn>(
        ::dlsym(metal_handle, "MTLCreateSystemDefaultDevice"));
    if (create_device == nullptr) {
        reject_immutable_install();
        return;
    }
    void* device = create_device();
    if (device == nullptr) {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: MTLCreateSystemDefaultDevice returned NULL\n");
        reject_immutable_install();
        return;
    }
    void* release_selector = objc.sel_register_name("release");
    ScopedObjcRelease device_owner{
        &objc, release_selector, device};

    void* sel_source = objc.sel_register_name("newLibraryWithSource:options:error:");
    void* sel_source_async =
        objc.sel_register_name("newLibraryWithSource:options:completionHandler:");
    state.sel_new_library_with_source = sel_source;
    state.sel_new_library_with_source_async = sel_source_async;
    state.sel_utf8_string = objc.sel_register_name("UTF8String");
    state.sel_new_library_with_data_error = objc.sel_register_name("newLibraryWithData:error:");
    using RegistryFn = unsigned long long (*)(void*, void*);
    state.loader_registry_id = static_cast<std::uint64_t>(
        objc_msg_cast<RegistryFn>(objc.msg_send)(
            device, objc.sel_register_name("registryID")));
    if (immutable_fail_closed && state.loader_registry_id == 0u) {
        rollback_swizzles();
        reject_immutable_install();
        return;
    }
    void* cls = objc.object_get_class(device);
    while (cls != nullptr
           && (installed_method == nullptr
               || installed_method_async == nullptr)) {
        if (installed_method == nullptr) {
            installed_method = objc.class_get_instance_method(cls, sel_source);
        }
        if (installed_method_async == nullptr) {
            installed_method_async =
                objc.class_get_instance_method(cls, sel_source_async);
        }
        cls = objc.class_get_superclass(cls);
    }
    if (installed_method == nullptr && installed_method_async == nullptr) {
            std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: newLibraryWithSource methods NOT FOUND on device class\n");
        rollback_swizzles();
        reject_immutable_install();
        return;
    }
    if (immutable_fail_closed
        && (installed_method == nullptr || installed_method_async == nullptr)) {
        rollback_swizzles();
        reject_immutable_install();
        return;
    }

    state.objc = objc;
    state.metallib = std::move(metallib);
    if (installed_method != nullptr) {
        state.original_new_library_with_source =
            objc.method_set_implementation(
                installed_method,
                reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source));
    }
    if (installed_method_async != nullptr) {
        state.original_new_library_with_source_async =
            objc.method_set_implementation(
                installed_method_async,
                reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source_async));
    }
    const bool any_installed =
        state.original_new_library_with_source != nullptr
        || state.original_new_library_with_source_async != nullptr;
    const bool every_present_method_installed =
        (installed_method == nullptr
         || state.original_new_library_with_source != nullptr)
        && (installed_method_async == nullptr
            || state.original_new_library_with_source_async != nullptr);
    state.installed = any_installed
        && (!immutable_fail_closed
            || (every_present_method_installed
                && state.original_new_library_with_source != nullptr
                && state.original_new_library_with_source_async != nullptr));
    state.cached_methods_present =
        static_cast<std::uint64_t>(installed_method != nullptr)
        + static_cast<std::uint64_t>(installed_method_async != nullptr);
    state.cached_wrapper_methods = 0u;
    if (installed_method != nullptr
        && objc.method_get_implementation(installed_method)
            == reinterpret_cast<void*>(
                &bioseq_moltenvk_new_library_with_source)) {
        ++state.cached_wrapper_methods;
    }
    if (installed_method_async != nullptr
        && objc.method_get_implementation(installed_method_async)
            == reinterpret_cast<void*>(
                &bioseq_moltenvk_new_library_with_source_async)) {
        ++state.cached_wrapper_methods;
    }
    state.cached_binding_validated = state.installed
        && state.loader_registry_id != 0u
        && state.cached_methods_present == 2u
        && state.cached_wrapper_methods == state.cached_methods_present;
    if (immutable_fail_closed && !state.cached_binding_validated) {
        rollback_swizzles();
        reject_immutable_install();
        return;
    }
    if (!state.installed) {
        rollback_swizzles();
        reject_immutable_install();
        return;
    }
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] cmd-interpose: swizzle installed=%d sync_original=%p async_original=%p\n",
                 state.installed ? 1 : 0,
                 state.original_new_library_with_source,
                 state.original_new_library_with_source_async);
    if (state.installed) {
        try {
            bioseq_log<DBG>(
                "[vulkan_runtime] installed MoltenVK command-source interpose immutable_fail_closed={} path={}",
                immutable_fail_closed,
                path != nullptr ? path : "(none)");
        } catch (...) {
        }
    }
    } catch (...) {
        rollback_swizzles();
        state.sticky_failure.store(true, std::memory_order_release);
    }
}

struct CommandInterposeDeviceBinding {
    std::uint64_t registry_id = 0u;
    std::uint64_t methods_present = 0u;
    std::uint64_t wrapper_methods = 0u;
};

/// Rare PSO-creation proof: bind the receipt to the exact intercepted device
/// and its live method table. Submit/wait gates consume cached scalar evidence;
/// this two-selector reflection runs only while creating a pipeline receipt.
[[nodiscard]] CommandInterposeDeviceBinding
command_interpose_device_binding(void* device) noexcept {
    auto& state = moltenvk_command_interpose_state();
    CommandInterposeDeviceBinding result{};
    if (device == nullptr || !state.installed
        || state.objc.msg_send == nullptr
        || state.objc.object_get_class == nullptr
        || state.objc.class_get_instance_method == nullptr
        || state.objc.class_get_superclass == nullptr
        || state.objc.method_get_implementation == nullptr) {
        return result;
    }
    using RegistryFn = unsigned long long (*)(void*, void*);
    result.registry_id = static_cast<std::uint64_t>(
        objc_msg_cast<RegistryFn>(state.objc.msg_send)(
            device, state.objc.sel_register_name("registryID")));
    const std::array selectors{
        std::pair{
            state.sel_new_library_with_source,
            reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source)},
        std::pair{
            state.sel_new_library_with_source_async,
            reinterpret_cast<void*>(
                &bioseq_moltenvk_new_library_with_source_async)},
    };
    void* root = state.objc.object_get_class(device);
    for (const auto& [selector, expected_imp] : selectors) {
        void* method = nullptr;
        for (void* cursor = root; cursor != nullptr && method == nullptr;
             cursor = state.objc.class_get_superclass(cursor)) {
            method = state.objc.class_get_instance_method(cursor, selector);
        }
        if (method == nullptr) continue;
        ++result.methods_present;
        if (state.objc.method_get_implementation(method) == expected_imp) {
            ++result.wrapper_methods;
        }
    }
    return result;
}

struct ActiveMetalArchivePipelineScope;

struct MetalPipelineArchiveInterpose {
    ObjcRuntimeFns objc{};
    std::shared_ptr<const RuntimeMetalArchiveConfigurationImage>
        configuration_image;
    std::array<std::uint8_t, VK_UUID_SIZE> runtime_vulkan_device_uuid{};
    RuntimeVulkanDeviceIdentity runtime_vulkan_device_identity{};
    std::uint64_t runtime_metal_registry_id = 0u;
    RuntimeMetalArchiveSetIdentity configured_identity{};
    RuntimeMetalArchiveSetIdentity loaded_identity{};
    // Declared before retained Objective-C archive ownership so teardown
    // releases the NSArray/archives before returning their residency budget.
    bioseq::parallelism::MemoryPermit archive_residency_permit;
    std::uint64_t archive_residency_peak_bytes = 0u;
    std::uint64_t archive_residency_retained_bytes = 0u;
    void* loader_device = nullptr;
    void* archive_array = nullptr;
    RuntimeMetalArchiveProcessQualification process_qualification{};
    void* interposed_new_pipeline_async_method = nullptr;
    void* interposed_new_pipeline_sync_method = nullptr;
    void* original_new_pipeline_async = nullptr;
    void* original_new_pipeline_sync = nullptr;
    void* sel_new_pipeline_sync = nullptr;
    void* sel_set_binary_archives = nullptr;
    void* sel_compute_function = nullptr;
    void* sel_name = nullptr;
    void* sel_utf8_string = nullptr;
    void* sel_error_domain = nullptr;
    void* sel_error_code = nullptr;
    void* sel_count = nullptr;
    void* sel_registry_id = nullptr;
    void* sel_release = nullptr;
    void* sel_max_total_threads = nullptr;
    void* sel_threadgroup_multiple = nullptr;
    std::uint64_t loader_registry_id = 0u;
    std::uint64_t loaded_pre_post_identity_checks = 0u;
    bool configuration_validated = false;
    bool installed = false;
    std::atomic<bool> sticky_failure{false};
    std::mutex failure_mu;
    std::string failure_reason;
    std::atomic<std::uint64_t> next_scope_identity{1u};
    std::atomic<std::uint64_t> next_receipt_identity{1u};
    std::mutex active_scope_mu;
    std::shared_ptr<ActiveMetalArchivePipelineScope> active_scope;
    std::atomic<std::uint64_t> outside_scope_interceptions{0u};

    ~MetalPipelineArchiveInterpose() {
        if (sel_release != nullptr && objc.msg_send != nullptr) {
            using ReleaseFn = void (*)(void*, void*);
            if (archive_array != nullptr) {
                objc_msg_cast<ReleaseFn>(objc.msg_send)(
                    archive_array, sel_release);
                archive_array = nullptr;
            }
            if (loader_device != nullptr) {
                objc_msg_cast<ReleaseFn>(objc.msg_send)(
                    loader_device, sel_release);
                loader_device = nullptr;
            }
        }
    }
};

MetalPipelineArchiveInterpose& metal_pipeline_archive_interpose_state() noexcept {
    static MetalPipelineArchiveInterpose state;
    return state;
}

void rollback_metal_archive_loaded_state(
    MetalPipelineArchiveInterpose& state) noexcept {
    if (state.archive_array != nullptr && state.sel_release != nullptr
        && state.objc.msg_send != nullptr) {
        using ReleaseFn = void (*)(void*, void*);
        objc_msg_cast<ReleaseFn>(state.objc.msg_send)(
            state.archive_array, state.sel_release);
        state.archive_array = nullptr;
    }
    state.archive_residency_permit.release();
    state.archive_residency_peak_bytes = 0u;
    state.archive_residency_retained_bytes = 0u;
    state.loaded_identity = {};
    state.loaded_pre_post_identity_checks = 0u;
    state.process_qualification = {};
}

void rollback_metal_pipeline_archive_interpose_install(
    MetalPipelineArchiveInterpose& state) noexcept {
    if (state.objc.method_set_implementation != nullptr) {
        if (state.interposed_new_pipeline_async_method != nullptr
            && state.original_new_pipeline_async != nullptr) {
            (void)state.objc.method_set_implementation(
                state.interposed_new_pipeline_async_method,
                state.original_new_pipeline_async);
        }
        if (state.interposed_new_pipeline_sync_method != nullptr
            && state.original_new_pipeline_sync != nullptr) {
            (void)state.objc.method_set_implementation(
                state.interposed_new_pipeline_sync_method,
                state.original_new_pipeline_sync);
        }
    }
    state.installed = false;
    state.interposed_new_pipeline_async_method = nullptr;
    state.interposed_new_pipeline_sync_method = nullptr;
    state.original_new_pipeline_async = nullptr;
    state.original_new_pipeline_sync = nullptr;
    rollback_metal_archive_loaded_state(state);
    if (state.loader_device != nullptr && state.sel_release != nullptr
        && state.objc.msg_send != nullptr) {
        using ReleaseFn = void (*)(void*, void*);
        objc_msg_cast<ReleaseFn>(state.objc.msg_send)(
            state.loader_device, state.sel_release);
    }
    state.loader_device = nullptr;
    state.configuration_image.reset();
    state.configured_identity = {};
    state.configuration_validated = false;
    state.runtime_vulkan_device_uuid = {};
    state.runtime_vulkan_device_identity = {};
    state.runtime_metal_registry_id = 0u;
    state.loader_registry_id = 0u;
    state.sel_new_pipeline_sync = nullptr;
    state.sel_set_binary_archives = nullptr;
    state.sel_compute_function = nullptr;
    state.sel_name = nullptr;
    state.sel_utf8_string = nullptr;
    state.sel_error_domain = nullptr;
    state.sel_error_code = nullptr;
    state.sel_count = nullptr;
    state.sel_registry_id = nullptr;
    state.sel_release = nullptr;
    state.sel_max_total_threads = nullptr;
    state.sel_threadgroup_multiple = nullptr;
    state.objc = {};
}

[[nodiscard]] std::string bioseq_ns_string_utf8(
    ObjcRuntimeFns& objc,
    void* ns_string,
    void* sel_utf8_string)
{
    if (ns_string == nullptr || sel_utf8_string == nullptr) {
        return {};
    }
    using Utf8Fn = const char* (*)(void*, void*);
    const char* text = objc_msg_cast<Utf8Fn>(objc.msg_send)(ns_string, sel_utf8_string);
    return text == nullptr ? std::string{} : std::string(text);
}

[[nodiscard]] void* bioseq_make_ns_string(ObjcRuntimeFns& objc, std::string_view value)
{
    void* ns_string_cls = objc.get_class("NSString");
    if (ns_string_cls == nullptr) {
        return nullptr;
    }
    std::string owned(value);
    using StringFn = void* (*)(void*, void*, const char*);
    return objc_msg_cast<StringFn>(objc.msg_send)(
        ns_string_cls,
        objc.sel_register_name("stringWithUTF8String:"),
        owned.c_str());
}

[[nodiscard]] void* bioseq_make_file_url(ObjcRuntimeFns& objc, const std::filesystem::path& path)
{
    void* url_cls = objc.get_class("NSURL");
    void* ns_path = bioseq_make_ns_string(objc, path.string());
    if (url_cls == nullptr || ns_path == nullptr) {
        return nullptr;
    }
    using UrlFn = void* (*)(void*, void*, void*);
    return objc_msg_cast<UrlFn>(objc.msg_send)(
        url_cls,
        objc.sel_register_name("fileURLWithPath:"),
        ns_path);
}

[[nodiscard]] std::string bioseq_ns_error_description(ObjcRuntimeFns& objc, void* error)
{
    if (error == nullptr) {
        return {};
    }
    using GetObjFn = void* (*)(void*, void*);
    void* desc = objc_msg_cast<GetObjFn>(objc.msg_send)(
        error,
        objc.sel_register_name("localizedDescription"));
    return bioseq_ns_string_utf8(objc, desc, objc.sel_register_name("UTF8String"));
}

void mark_metal_archive_sticky_failure(
    MetalPipelineArchiveInterpose& state,
    std::string reason) noexcept {
    state.sticky_failure.store(true, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> guard(state.failure_mu);
        if (state.failure_reason.empty()) state.failure_reason = std::move(reason);
    } catch (...) {
        // The atomic verdict is authoritative even if diagnostics cannot be
        // allocated on this already-failing path.
    }
}

void mark_metal_archive_sticky_failure_literal(
    MetalPipelineArchiveInterpose& state,
    const char* reason) noexcept {
    state.sticky_failure.store(true, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> guard(state.failure_mu);
        if (state.failure_reason.empty()) state.failure_reason.assign(reason);
    } catch (...) {
    }
}

[[nodiscard]] std::string metal_archive_failure_reason(
    MetalPipelineArchiveInterpose& state) {
    std::lock_guard<std::mutex> guard(state.failure_mu);
    return state.failure_reason.empty()
        ? std::string{"immutable Metal archive proof failed"}
        : state.failure_reason;
}

struct MetalArchiveFileSnapshot {
    RuntimeMetalArchiveSpec actual{};
    std::uint64_t device = 0u;
    std::uint64_t inode = 0u;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;
    std::int64_t changed_seconds = 0;
    std::int64_t changed_nanoseconds = 0;
    std::uint64_t streamed_bytes = 0u;
    std::uint64_t scratch_capacity_bytes = 0u;
    bool exact_seed_compared = false;

    [[nodiscard]] bool operator==(
        const MetalArchiveFileSnapshot&) const noexcept = default;
};

[[nodiscard]] std::expected<MetalArchiveFileSnapshot, std::string>
capture_metal_archive_file_snapshot(
    const std::filesystem::path& physical_path,
    const RuntimeMetalArchiveSpec& expected,
    std::span<const std::byte> exact_seed_bytes = {}) {
    const std::filesystem::path path = physical_path;
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(path, ec);
    if (ec || canonical != path) {
        return std::unexpected(fmt::format(
            "private Metal archive snapshot path is not canonical for {}",
            expected.artifact_filename));
    }

    struct stat before{};
    if (::lstat(path.c_str(), &before) != 0
        || !S_ISREG(before.st_mode)) {
        return std::unexpected(fmt::format(
            "sealed Metal archive is missing, non-regular, or a symlink: {}",
            path.string()));
    }
    if (before.st_size <= 0) {
        return std::unexpected(fmt::format(
            "sealed Metal archive is empty: {}", path.string()));
    }
    if (static_cast<std::uint64_t>(before.st_size) != expected.size
        || (!exact_seed_bytes.empty()
            && exact_seed_bytes.size() != expected.size)) {
        return std::unexpected(fmt::format(
            "private Metal archive size differs from retained seed for {}",
            expected.artifact_filename));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(fmt::format(
            "cannot open private Metal archive {}", path.string()));
    }
    constexpr std::size_t scratch_bytes = 64u * 1024u;
    std::array<std::byte, scratch_bytes> scratch{};
    sealed_shader_cache_detail::Sha256 hasher;
    std::uint64_t streamed_bytes = 0u;
    bool seed_matches = true;
    for (;;) {
        input.read(
            reinterpret_cast<char*>(scratch.data()),
            static_cast<std::streamsize>(scratch.size()));
        const std::streamsize count = input.gcount();
        if (count < 0) {
            return std::unexpected(fmt::format(
                "cannot stream private Metal archive {}", path.string()));
        }
        if (count == 0) break;
        const std::size_t chunk_bytes = static_cast<std::size_t>(count);
        const std::span<const std::byte> chunk{
            scratch.data(), chunk_bytes};
        hasher.update(chunk);
        if (!exact_seed_bytes.empty()) {
            const std::size_t offset = static_cast<std::size_t>(streamed_bytes);
            if (offset > exact_seed_bytes.size()
                || chunk_bytes > exact_seed_bytes.size() - offset
                || !std::equal(
                    chunk.begin(), chunk.end(),
                    exact_seed_bytes.begin()
                        + static_cast<std::ptrdiff_t>(offset))) {
                seed_matches = false;
            }
        }
        streamed_bytes += static_cast<std::uint64_t>(chunk_bytes);
        if (!input && !input.eof()) {
            return std::unexpected(fmt::format(
                "cannot stream private Metal archive {}", path.string()));
        }
    }
    if (streamed_bytes != expected.size || !seed_matches) {
        return std::unexpected(fmt::format(
            "private Metal archive bytes differ from retained seed for {}",
            expected.artifact_filename));
    }
    struct stat after{};
    if (::lstat(path.c_str(), &after) != 0
        || !S_ISREG(after.st_mode)
        || before.st_dev != after.st_dev
        || before.st_ino != after.st_ino
        || before.st_size != after.st_size
        || before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec
        || before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec
        || before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec
        || before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec) {
        return std::unexpected(fmt::format(
            "sealed Metal archive changed while hashing: {}",
            path.string()));
    }
    RuntimeMetalArchiveSpec actual{
        .artifact_ordinal = expected.artifact_ordinal,
        .artifact_filename = expected.artifact_filename,
        .size = static_cast<std::uint64_t>(after.st_size),
        .sha256 = hasher.finalize(),
        .negative_error_domain = expected.negative_error_domain,
        .negative_error_code = expected.negative_error_code,
    };
    if (actual != expected) {
        return std::unexpected(fmt::format(
            "private Metal archive size or SHA-256 mismatch for {}",
            expected.artifact_filename));
    }
    return MetalArchiveFileSnapshot{
        .actual = std::move(actual),
        .device = static_cast<std::uint64_t>(after.st_dev),
        .inode = static_cast<std::uint64_t>(after.st_ino),
        .modified_seconds = after.st_mtimespec.tv_sec,
        .modified_nanoseconds = after.st_mtimespec.tv_nsec,
        .changed_seconds = after.st_ctimespec.tv_sec,
        .changed_nanoseconds = after.st_ctimespec.tv_nsec,
        .streamed_bytes = streamed_bytes,
        .scratch_capacity_bytes = scratch.size(),
        .exact_seed_compared = !exact_seed_bytes.empty(),
    };
}

class PrivateMetalArchiveSnapshot {
public:
    PrivateMetalArchiveSnapshot() = default;
    explicit PrivateMetalArchiveSnapshot(std::filesystem::path path)
        : path_(std::move(path)) {}
    PrivateMetalArchiveSnapshot(const PrivateMetalArchiveSnapshot&) = delete;
    PrivateMetalArchiveSnapshot& operator=(
        const PrivateMetalArchiveSnapshot&) = delete;
    PrivateMetalArchiveSnapshot(
        PrivateMetalArchiveSnapshot&& other) noexcept
        : path_(std::exchange(other.path_, {})) {}
    PrivateMetalArchiveSnapshot& operator=(
        PrivateMetalArchiveSnapshot&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }
    ~PrivateMetalArchiveSnapshot() { cleanup(); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::filesystem::path release_path() noexcept {
        return std::exchange(path_, {});
    }

private:
    void cleanup() noexcept {
        if (!path_.empty()) {
            (void)::unlink(path_.c_str());
            path_.clear();
        }
    }

    std::filesystem::path path_;
};
static_assert(std::is_nothrow_move_constructible_v<PrivateMetalArchiveSnapshot>);

class PrivateSnapshotFileDescriptor {
public:
    explicit PrivateSnapshotFileDescriptor(int fd) noexcept : fd_(fd) {}
    PrivateSnapshotFileDescriptor(const PrivateSnapshotFileDescriptor&) = delete;
    PrivateSnapshotFileDescriptor& operator=(
        const PrivateSnapshotFileDescriptor&) = delete;
    ~PrivateSnapshotFileDescriptor() noexcept {
        if (fd_ >= 0) (void)::close(fd_);
    }

    [[nodiscard]] int get() const noexcept { return fd_; }

    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    [[nodiscard]] int close_now() noexcept {
        const int fd = std::exchange(fd_, -1);
        if (fd < 0 || ::close(fd) == 0) return 0;
        return errno;
    }

private:
    int fd_ = -1;
};

class PrivateSnapshotUnlinkGuard {
public:
    explicit PrivateSnapshotUnlinkGuard(const char* path) noexcept
        : path_(path) {}
    PrivateSnapshotUnlinkGuard(const PrivateSnapshotUnlinkGuard&) = delete;
    PrivateSnapshotUnlinkGuard& operator=(
        const PrivateSnapshotUnlinkGuard&) = delete;
    ~PrivateSnapshotUnlinkGuard() noexcept {
        if (path_ != nullptr) (void)::unlink(path_);
    }

    void release() noexcept { path_ = nullptr; }

private:
    const char* path_ = nullptr;
};

[[nodiscard]] std::expected<PrivateMetalArchiveSnapshot, std::string>
write_private_metal_archive_snapshot(
    std::string_view artifact_filename,
    std::span<const std::byte> bytes,
    const std::filesystem::path& temp_parent_override = {}) {
    std::error_code temp_error;
    const std::filesystem::path temp_dir = temp_parent_override.empty()
        ? std::filesystem::temp_directory_path(temp_error)
        : temp_parent_override;
    if (temp_error || !temp_dir.is_absolute()) {
        return std::unexpected(fmt::format(
            "cannot resolve private Metal archive snapshot directory for {}: {}",
            artifact_filename,
            temp_error ? temp_error.message() : "path is not absolute"));
    }
    std::error_code canonical_error;
    const std::filesystem::path canonical_temp_dir =
        std::filesystem::canonical(temp_dir, canonical_error);
    if (canonical_error || !canonical_temp_dir.is_absolute()) {
        return std::unexpected(fmt::format(
            "cannot canonicalize private Metal archive snapshot directory for {}: {}",
            artifact_filename,
            canonical_error
                ? canonical_error.message() : "path is not absolute"));
    }
    std::filesystem::path pattern = canonical_temp_dir
        / (".bioseq-metalar-snapshot-" + std::string(artifact_filename)
           + "-XXXXXX");
    std::string owned_pattern = pattern.string();
    std::vector<char> writable(
        owned_pattern.begin(), owned_pattern.end());
    writable.push_back('\0');
    const int fd = ::mkstemp(writable.data());
    if (fd < 0) {
        return std::unexpected(fmt::format(
            "cannot create private Metal archive snapshot for {}: {}",
            artifact_filename,
            std::strerror(errno)));
    }
    PrivateSnapshotFileDescriptor fd_owner{fd};
    PrivateSnapshotUnlinkGuard unlink_owner{writable.data()};
    std::filesystem::path snapshot_path(writable.data());
    if (::fchmod(fd_owner.get(), S_IRUSR | S_IWUSR) != 0) {
        return std::unexpected(fmt::format(
            "cannot restrict private Metal archive snapshot {}: {}",
            snapshot_path.string(),
            std::strerror(errno)));
    }
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(
            fd_owner.get(),
            bytes.data() + offset,
            bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            return std::unexpected(fmt::format(
                "cannot write private Metal archive snapshot {}: {}",
                snapshot_path.string(),
                std::strerror(errno)));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd_owner.get()) != 0) {
        return std::unexpected(fmt::format(
            "cannot fsync private Metal archive snapshot {}: {}",
            snapshot_path.string(),
            std::strerror(errno)));
    }
    const int close_error = fd_owner.close_now();
    if (close_error != 0) {
        return std::unexpected(fmt::format(
            "cannot close private Metal archive snapshot {}: {}",
            snapshot_path.string(),
            std::strerror(close_error)));
    }
    PrivateMetalArchiveSnapshot result{std::move(snapshot_path)};
    unlink_owner.release();
    return std::expected<PrivateMetalArchiveSnapshot, std::string>{
        std::in_place, std::move(result)};
}

class PrivateMetalArchiveProcessDirectory {
public:
    PrivateMetalArchiveProcessDirectory() = default;
    explicit PrivateMetalArchiveProcessDirectory(std::filesystem::path path)
        : path_(std::move(path)) {}
    PrivateMetalArchiveProcessDirectory(
        const PrivateMetalArchiveProcessDirectory&) = delete;
    PrivateMetalArchiveProcessDirectory& operator=(
        const PrivateMetalArchiveProcessDirectory&) = delete;
    PrivateMetalArchiveProcessDirectory(
        PrivateMetalArchiveProcessDirectory&& other) noexcept
        : path_(std::exchange(other.path_, {})) {}
    ~PrivateMetalArchiveProcessDirectory() noexcept {
        if (!path_.empty()) (void)::rmdir(path_.c_str());
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] int remove_now() noexcept {
        if (path_.empty()) return 0;
        if (::rmdir(path_.c_str()) == 0) {
            path_.clear();
            return 0;
        }
        return errno;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::expected<PrivateMetalArchiveProcessDirectory, std::string>
make_private_metal_archive_process_directory()
{
    std::error_code temp_error;
    const std::filesystem::path temp_parent =
        std::filesystem::temp_directory_path(temp_error);
    std::error_code canonical_error;
    const std::filesystem::path canonical_parent = temp_error
        ? std::filesystem::path{}
        : std::filesystem::canonical(temp_parent, canonical_error);
    if (temp_error || canonical_error || !canonical_parent.is_absolute()) {
        return std::unexpected(
            "cannot resolve private Metal archive process directory");
    }
    std::string pattern =
        (canonical_parent / ".bioseq-metalar-process-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = ::mkdtemp(writable.data());
    if (created == nullptr) {
        return std::unexpected(fmt::format(
            "cannot create private Metal archive process directory: {}",
            std::strerror(errno)));
    }
    PrivateMetalArchiveProcessDirectory owner{
        std::filesystem::path(created)};
    if (::chmod(owner.path().c_str(), S_IRWXU) != 0) {
        return std::unexpected(fmt::format(
            "cannot restrict private Metal archive process directory: {}",
            std::strerror(errno)));
    }
    struct stat status {};
    if (::lstat(owner.path().c_str(), &status) != 0
        || !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)
        || status.st_uid != ::geteuid()
        || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(
            "private Metal archive process directory identity is invalid");
    }
    return owner;
}

class OpenPrivateMetalArchive {
public:
    OpenPrivateMetalArchive() = default;
    OpenPrivateMetalArchive(
        int fd,
        std::filesystem::path path,
        sealed_shader_cache_detail::RegularFileIdentity identity) noexcept
        : fd_(fd), path_(std::move(path)), identity_(identity) {}
    OpenPrivateMetalArchive(const OpenPrivateMetalArchive&) = delete;
    OpenPrivateMetalArchive& operator=(const OpenPrivateMetalArchive&) = delete;
    OpenPrivateMetalArchive(OpenPrivateMetalArchive&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          path_(std::exchange(other.path_, {})),
          identity_(other.identity_),
          linked_(std::exchange(other.linked_, false)) {}
    ~OpenPrivateMetalArchive() noexcept {
        if (linked_ && !path_.empty()) (void)::unlink(path_.c_str());
        if (fd_ >= 0) (void)::close(fd_);
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] const sealed_shader_cache_detail::RegularFileIdentity&
    identity() const noexcept { return identity_; }

    [[nodiscard]] bool path_and_fd_identity_match() const noexcept {
        if (fd_ < 0 || path_.empty() || !linked_) return false;
        struct stat fd_status {};
        struct stat path_status {};
        return ::fstat(fd_, &fd_status) == 0
            && ::lstat(path_.c_str(), &path_status) == 0
            && S_ISREG(fd_status.st_mode) && S_ISREG(path_status.st_mode)
            && !S_ISLNK(path_status.st_mode)
            && sealed_shader_cache_detail::regular_file_identity(fd_status)
                == identity_
            && sealed_shader_cache_detail::regular_file_identity(path_status)
                == identity_;
    }

    [[nodiscard]] std::expected<void, std::string>
    unlink_close_and_require_absent()
    {
        if (!path_and_fd_identity_match()) {
            return std::unexpected(
                "private Metal archive identity changed before unlink");
        }
        if (::unlink(path_.c_str()) != 0) {
            return std::unexpected(fmt::format(
                "cannot unlink private Metal archive immediately: {}",
                std::strerror(errno)));
        }
        linked_ = false;
        struct stat recreated {};
        errno = 0;
        if (::lstat(path_.c_str(), &recreated) == 0 || errno != ENOENT) {
            return std::unexpected(
                "private Metal archive name remained or was recreated after unlink");
        }
        struct stat fd_status {};
        if (::fstat(fd_, &fd_status) != 0
            || sealed_shader_cache_detail::regular_file_identity(fd_status)
                != identity_) {
            return std::unexpected(
                "unlinked private Metal archive descriptor identity changed");
        }
        const int fd = std::exchange(fd_, -1);
        if (::close(fd) != 0) {
            return std::unexpected(fmt::format(
                "cannot close unlinked private Metal archive descriptor: {}",
                std::strerror(errno)));
        }
        path_.clear();
        return {};
    }

private:
    int fd_ = -1;
    std::filesystem::path path_;
    sealed_shader_cache_detail::RegularFileIdentity identity_{};
    bool linked_ = true;
};

[[nodiscard]] std::expected<OpenPrivateMetalArchive, std::string>
write_open_private_metal_archive(
    const std::filesystem::path& directory,
    std::size_t ordinal,
    std::span<const std::byte> bytes)
{
    const std::filesystem::path path = directory
        / ("archive-" + std::to_string(ordinal) + ".metalar");
    const int fd = ::open(
        path.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return std::unexpected(fmt::format(
            "cannot create open private Metal archive: {}",
            std::strerror(errno)));
    }
    PrivateSnapshotFileDescriptor fd_owner{fd};
    PrivateSnapshotUnlinkGuard unlink_owner{path.c_str()};
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        return std::unexpected(
            "cannot restrict open private Metal archive");
    }
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(
            fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            return std::unexpected(
                "cannot write open private Metal archive");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0) {
        return std::unexpected("cannot fsync open private Metal archive");
    }
    struct stat status {};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_uid != ::geteuid()
        || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0
        || status.st_size < 0
        || static_cast<std::uint64_t>(status.st_size) != bytes.size()) {
        return std::unexpected(
            "open private Metal archive identity is invalid");
    }
    OpenPrivateMetalArchive result{
        fd_owner.get(), path,
        sealed_shader_cache_detail::regular_file_identity(status)};
    (void)fd_owner.release();
    unlink_owner.release();
    return result;
}

[[nodiscard]] std::expected<std::uint64_t, std::string>
hash_open_private_metal_archive_once(
    OpenPrivateMetalArchive& image,
    const RuntimeMetalArchiveSpec& expected,
    std::span<const std::byte> exact_seed_bytes)
{
    if (!image.path_and_fd_identity_match()
        || exact_seed_bytes.size() != expected.size
        || ::lseek(image.fd(), 0, SEEK_SET) < 0) {
        return std::unexpected(
            "private Metal archive is not stable before qualification hash");
    }
    sealed_shader_cache_detail::Sha256 hasher;
    std::array<std::byte, 64u << 10u> scratch{};
    std::uint64_t streamed = 0u;
    for (;;) {
        const ssize_t count = ::read(
            image.fd(), scratch.data(), scratch.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            return std::unexpected(
                "private Metal archive qualification hash read failed");
        }
        if (count == 0) break;
        const std::size_t chunk = static_cast<std::size_t>(count);
        if (streamed > exact_seed_bytes.size()
            || chunk > exact_seed_bytes.size() - streamed
            || !std::equal(
                scratch.begin(),
                scratch.begin() + static_cast<std::ptrdiff_t>(chunk),
                exact_seed_bytes.begin()
                    + static_cast<std::ptrdiff_t>(streamed))) {
            return std::unexpected(
                "private Metal archive differs from retained seed bytes");
        }
        hasher.update(std::span<const std::byte>{scratch.data(), chunk});
        streamed += chunk;
    }
    if (streamed != expected.size || hasher.finalize() != expected.sha256
        || !image.path_and_fd_identity_match()) {
        return std::unexpected(
            "private Metal archive qualification hash mismatch");
    }
    return streamed;
}

[[nodiscard]] void* load_metal_binary_archive(
    MetalPipelineArchiveInterpose& state,
    void* loader_device,
    const std::filesystem::path& path)
{
    void* desc_cls = state.objc.get_class("MTLBinaryArchiveDescriptor");
    if (desc_cls == nullptr || loader_device == nullptr) {
        return nullptr;
    }
    using NewFn = void* (*)(void*, void*);
    void* desc = objc_msg_cast<NewFn>(state.objc.msg_send)(
        desc_cls,
        state.objc.sel_register_name("new"));
    ScopedObjcRelease desc_owner{
        &state.objc, state.sel_release, desc};
    void* url = bioseq_make_file_url(state.objc, path);
    if (desc == nullptr || url == nullptr) {
        return nullptr;
    }
    using SetObjFn = void (*)(void*, void*, void*);
    objc_msg_cast<SetObjFn>(state.objc.msg_send)(
        desc,
        state.objc.sel_register_name("setUrl:"),
        url);
    void* error = nullptr;
    using NewArchiveFn = void* (*)(void*, void*, void*, void**);
    void* archive = objc_msg_cast<NewArchiveFn>(state.objc.msg_send)(
        loader_device,
        state.objc.sel_register_name("newBinaryArchiveWithDescriptor:error:"),
        desc,
        &error);
    if (archive == nullptr) {
        bioseq_log<DBG>(
            "[vulkan_runtime] failed to load Metal binary archive {}: {}",
            path.string(),
            bioseq_ns_error_description(state.objc, error));
    }
    return archive;
}

[[nodiscard]] bool load_exact_metal_binary_archives(
    MetalPipelineArchiveInterpose& state,
    void* loader_device) {
    state.loaded_pre_post_identity_checks = 0u;
    if (!state.configuration_image
        || !state.configuration_image->accepted()) {
        mark_metal_archive_sticky_failure(
            state, "immutable Metal archive configuration image is absent");
        return false;
    }
    const RuntimeMetalArchiveConfiguration& configuration =
        state.configuration_image->configuration();
    const RuntimeMetalArchiveSetIdentity expected_identity =
        runtime_metal_archive_set_identity(configuration.archives);
    const std::uint64_t residency_metadata =
        bioseq::parallelism::saturating_add(
            bioseq::parallelism::saturating_mul(
                expected_identity.archive_count,
                sizeof(RuntimeMetalArchiveSpec) + 2u * sizeof(void*)),
            2u * 1024u);
    const std::uint64_t residency_peak =
        bioseq::parallelism::saturating_mul(
            bioseq::parallelism::saturating_add(
                expected_identity.total_size, residency_metadata),
            bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    if (!expected_identity.accepted()
        || residency_peak == (std::numeric_limits<std::uint64_t>::max)()) {
        mark_metal_archive_sticky_failure(
            state, "immutable Metal archive residency admission saturated");
        return false;
    }
    bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
    auto residency_permit =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(residency_peak, &admission_failure);
    if (!residency_permit) {
        mark_metal_archive_sticky_failure(
            state,
            shader_cache_message(
                "immutable Metal archive residency admission denied before load: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(
                    admission_failure.reason),
                " requested=", admission_failure.requested_bytes));
        return false;
    }
    void* array_cls = state.objc.get_class("NSMutableArray");
    if (array_cls == nullptr) {
        mark_metal_archive_sticky_failure(
            state, "NSMutableArray is unavailable for immutable Metal archives");
        return false;
    }
    using AllocFn = void* (*)(void*, void*);
    using InitCapacityFn = void* (*)(void*, void*, unsigned long);
    void* allocated = objc_msg_cast<AllocFn>(state.objc.msg_send)(
        array_cls,
        state.objc.sel_register_name("alloc"));
    void* array = objc_msg_cast<InitCapacityFn>(state.objc.msg_send)(
        allocated,
        state.objc.sel_register_name("initWithCapacity:"),
        static_cast<unsigned long>(configuration.archives.size()));
    ScopedObjcRelease array_owner{
        &state.objc, state.sel_release, array};
    if (array == nullptr) {
        mark_metal_archive_sticky_failure(
            state, "cannot allocate exact immutable Metal archive array");
        return false;
    }

    auto private_directory = make_private_metal_archive_process_directory();
    if (!private_directory) {
        mark_metal_archive_sticky_failure(
            state, std::move(private_directory.error()));
        return false;
    }
    using AddObjectFn = void (*)(void*, void*, void*);
    std::uint64_t hashed_bytes = 0u;
    std::uint64_t private_files_created = 0u;
    std::uint64_t private_files_unlinked = 0u;
    for (std::size_t archive_index = 0u;
         archive_index < configuration.archives.size(); ++archive_index) {
        const RuntimeMetalArchiveSpec& expected =
            configuration.archives[archive_index];
        const SealedShaderCacheArtifactImage* seed_artifact =
            configuration.seed_image->artifact(
                expected.artifact_ordinal);
        if (seed_artifact == nullptr
            || seed_artifact->descriptor.kind
                != SealedShaderCacheArtifactKind::metal_binary_archive
            || seed_artifact->descriptor.filename
                != expected.artifact_filename
            || seed_artifact->descriptor.size != expected.size
            || seed_artifact->descriptor.sha256 != expected.sha256) {
            mark_metal_archive_sticky_failure(
                state,
                fmt::format(
                    "runtime Metal archive spec no longer matches retained seed bytes for {}",
                    expected.artifact_filename));
            return false;
        }
        auto private_image = write_open_private_metal_archive(
            private_directory->path(), archive_index, seed_artifact->bytes);
        if (!private_image) {
            mark_metal_archive_sticky_failure(
                state, std::move(private_image.error()));
            return false;
        }
        ++private_files_created;
        auto hashed = hash_open_private_metal_archive_once(
            *private_image, expected, seed_artifact->bytes);
        if (!hashed
            || hashed_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - (hashed ? *hashed : 0u)) {
            mark_metal_archive_sticky_failure(
                state,
                hashed ? "private Metal archive hashed byte count overflow"
                       : std::move(hashed.error()));
            return false;
        }
        hashed_bytes += *hashed;
        if (!private_image->path_and_fd_identity_match()) {
            mark_metal_archive_sticky_failure(
                state,
                "private Metal archive path/descriptor identity changed before synchronous load");
            return false;
        }
        if (void* archive = load_metal_binary_archive(
                state, loader_device, private_image->path());
            archive != nullptr) {
            ScopedObjcRelease archive_owner{
                &state.objc, state.sel_release, archive};
            if (!private_image->path_and_fd_identity_match()) {
                mark_metal_archive_sticky_failure(
                    state,
                    fmt::format(
                        "private Metal archive identity changed across synchronous load: {}",
                        expected.artifact_filename));
                return false;
            }
            objc_msg_cast<AddObjectFn>(state.objc.msg_send)(
                array,
                state.objc.sel_register_name("addObject:"),
                archive);
            auto unlinked =
                private_image->unlink_close_and_require_absent();
            if (!unlinked) {
                mark_metal_archive_sticky_failure(
                    state, std::move(unlinked.error()));
                return false;
            }
            ++private_files_unlinked;
            ++state.loaded_pre_post_identity_checks;
        } else {
            mark_metal_archive_sticky_failure(
                state,
                fmt::format(
                    "Metal rejected private exact-byte archive snapshot for {}",
                    expected.artifact_filename));
            return false;
        }
    }
    if (const int remove_error = private_directory->remove_now();
        remove_error != 0) {
        mark_metal_archive_sticky_failure(
            state,
            fmt::format(
                "private Metal archive process directory did not disappear: {}",
                std::strerror(remove_error)));
        return false;
    }
    state.loaded_identity = expected_identity;
    if (state.loaded_pre_post_identity_checks
        != expected_identity.archive_count) {
        mark_metal_archive_sticky_failure(
            state, "loaded Metal archive count differs from sealed expected set");
        return false;
    }
    using CopyFn = void* (*)(void*, void*);
    void* immutable_array = objc_msg_cast<CopyFn>(state.objc.msg_send)(
        array, state.objc.sel_register_name("copy"));
    ScopedObjcRelease immutable_array_owner{
        &state.objc, state.sel_release, immutable_array};
    using CountFn = unsigned long (*)(void*, void*);
    const std::uint64_t immutable_count = immutable_array == nullptr
        ? 0u
        : static_cast<std::uint64_t>(
            objc_msg_cast<CountFn>(state.objc.msg_send)(
                immutable_array, state.sel_count));
    if (immutable_array == nullptr
        || immutable_count != expected_identity.archive_count) {
        mark_metal_archive_sticky_failure(
            state, "cannot retain immutable Metal archive NSArray");
        return false;
    }
    const SealedShaderCacheManifest& manifest =
        configuration.seed_image->manifest();
    const SealedShaderCacheSeedReceipt& seed_receipt =
        configuration.seed_image->receipt();
    RuntimeMetalArchiveProcessQualification qualification{
        .version = 2u,
        .archive_set = expected_identity,
        .receipt_count = configuration.qualification_receipt_count,
        .hash_passes = 1u,
        .hashed_bytes = hashed_bytes,
        .private_files_created = private_files_created,
        .private_files_unlinked = private_files_unlinked,
        .live_private_paths = 0u,
        .pipeline_filesystem_hash_passes = 0u,
        .pipeline_filesystem_hashed_bytes = 0u,
        .helper_protocol_version = configuration.helper_protocol_version,
        .helper_build_identity = configuration.helper_build_identity,
        .helper_source = configuration.helper_source,
        .helper_executable_size = configuration.helper_executable_size,
        .helper_executable_sha256 = configuration.helper_executable_sha256,
        .helper_metal_registry_id = configuration.helper_metal_registry_id,
        .vulkan_device_uuid =
            state.runtime_vulkan_device_identity.device_uuid,
        .api_version = state.runtime_vulkan_device_identity.api_version,
        .driver_version = state.runtime_vulkan_device_identity.driver_version,
        .vendor_id = state.runtime_vulkan_device_identity.vendor_id,
        .device_id = state.runtime_vulkan_device_identity.device_id,
        .pipeline_cache_uuid =
            state.runtime_vulkan_device_identity.pipeline_cache_uuid,
        .seed_receipt_identity = seed_receipt.identity(),
        .seed_sha256 = manifest.seed_sha256,
        .seed_size = manifest.seed_size,
        .manifest_checksum = manifest.manifest_checksum,
        .saturated = false,
    };
    qualification.identity = runtime_metal_archive_process_qualification_identity(
        qualification);
    if (!qualification.accepted()) {
        mark_metal_archive_sticky_failure(
            state, "process Metal archive qualification is incomplete");
        return false;
    }
    state.archive_residency_peak_bytes = residency_peak;
    state.archive_residency_retained_bytes =
        bioseq::parallelism::saturating_add(
            expected_identity.total_size, residency_metadata);
    if (state.archive_residency_retained_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || state.archive_residency_retained_bytes
            > residency_permit->bytes()) {
        mark_metal_archive_sticky_failure(
            state, "Metal archive residency reconciliation saturated");
        state.archive_residency_retained_bytes = 0u;
        return false;
    }
    residency_permit->shrink_to(state.archive_residency_retained_bytes);
    state.archive_residency_permit = std::move(*residency_permit);
    state.archive_array = immutable_array_owner.release();
    state.process_qualification = std::move(qualification);
    std::fprintf(
        stderr,
        "[bioseq-gpu-diag] pso-interpose: loaded exact sealed archive set count=%llu bytes=%llu checksum=%llu\n",
        static_cast<unsigned long long>(state.loaded_identity.archive_count),
        static_cast<unsigned long long>(state.loaded_identity.total_size),
        static_cast<unsigned long long>(state.loaded_identity.ordered_checksum));
    bioseq_log<DBG>(
        "[vulkan_runtime] loaded exact sealed Metal archive set count={} bytes={} checksum={}",
        state.loaded_identity.archive_count,
        state.loaded_identity.total_size,
        state.loaded_identity.ordered_checksum);
    return true;
}

[[nodiscard]] std::expected<std::uint64_t, std::string>
verify_resident_metal_archive_snapshots(
    MetalPipelineArchiveInterpose& state,
    std::string_view phase) {
    if (!state.configuration_image || state.archive_array == nullptr
        || !state.process_qualification.accepted()) {
        return std::unexpected(
            fmt::format(
                "{} process Metal archive qualification is absent", phase));
    }
    if (state.loaded_identity != state.process_qualification.archive_set
        || state.process_qualification.live_private_paths != 0u
        || state.process_qualification.pipeline_filesystem_hash_passes != 0u
        || state.process_qualification.pipeline_filesystem_hashed_bytes != 0u) {
        return std::unexpected(fmt::format(
            "{} process Metal archive qualification identity mismatch",
            phase));
    }
    return 0u;
}

[[nodiscard]] std::string descriptor_function_name(MetalPipelineArchiveInterpose& state, void* desc)
{
    if (desc == nullptr || state.sel_compute_function == nullptr || state.sel_name == nullptr) {
        return {};
    }
    using GetObjFn = void* (*)(void*, void*);
    void* function = objc_msg_cast<GetObjFn>(state.objc.msg_send)(
        desc,
        state.sel_compute_function);
    void* name = function == nullptr
        ? nullptr
        : objc_msg_cast<GetObjFn>(state.objc.msg_send)(function, state.sel_name);
    return bioseq_ns_string_utf8(state.objc, name, state.sel_utf8_string);
}

struct MetalErrorIdentity {
    std::string domain;
    std::int64_t code = 0;
};

[[nodiscard]] MetalErrorIdentity metal_error_identity(
    MetalPipelineArchiveInterpose& state,
    void* error) {
    if (error == nullptr || state.sel_error_domain == nullptr
        || state.sel_error_code == nullptr) {
        return {};
    }
    using GetObjFn = void* (*)(void*, void*);
    using CodeFn = std::intptr_t (*)(void*, void*);
    void* domain = objc_msg_cast<GetObjFn>(state.objc.msg_send)(
        error, state.sel_error_domain);
    return MetalErrorIdentity{
        .domain = bioseq_ns_string_utf8(
            state.objc, domain, state.sel_utf8_string),
        .code = static_cast<std::int64_t>(
            objc_msg_cast<CodeFn>(state.objc.msg_send)(
                error, state.sel_error_code)),
    };
}

constexpr unsigned long kMetalPipelineFailOnBinaryArchiveMiss = 1ul << 2u;

struct ActiveMetalArchivePipelineScope {
    std::mutex mu;
    RuntimeMetalArchivePipelineAudit audit{};
    bool required = false;
    bool active = false;
    bool finished = false;
    std::string failure;
    std::uint64_t outside_scope_interceptions_at_begin = 0u;
};

[[nodiscard]] std::uint64_t next_nonzero_identity(
    std::atomic<std::uint64_t>& counter) noexcept {
    constexpr std::uint64_t exhausted =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t value = counter.load(std::memory_order_relaxed);
    while (value != 0u && value != exhausted) {
        if (counter.compare_exchange_weak(
                value, value + 1u,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return value;
        }
    }
    return 0u;
}

[[nodiscard]] std::uint64_t metal_device_registry_id(
    MetalPipelineArchiveInterpose& state,
    void* device) noexcept {
    if (device == nullptr || state.sel_registry_id == nullptr) return 0u;
    using RegistryFn = unsigned long long (*)(void*, void*);
    return static_cast<std::uint64_t>(
        objc_msg_cast<RegistryFn>(state.objc.msg_send)(
            device, state.sel_registry_id));
}

void release_objc_object(
    MetalPipelineArchiveInterpose& state,
    void* object) noexcept {
    if (object == nullptr || state.sel_release == nullptr) return;
    using ReleaseFn = void (*)(void*, void*);
    objc_msg_cast<ReleaseFn>(state.objc.msg_send)(object, state.sel_release);
}

[[nodiscard]] void* make_empty_metal_archive_array(
    MetalPipelineArchiveInterpose& state,
    void* device) noexcept {
    void* desc_cls = state.objc.get_class("MTLBinaryArchiveDescriptor");
    void* array_cls = state.objc.get_class("NSMutableArray");
    if (desc_cls == nullptr || array_cls == nullptr || device == nullptr) {
        return nullptr;
    }
    using NewFn = void* (*)(void*, void*);
    void* desc = objc_msg_cast<NewFn>(state.objc.msg_send)(
        desc_cls, state.objc.sel_register_name("new"));
    if (desc == nullptr) return nullptr;
    void* error = nullptr;
    using NewArchiveFn = void* (*)(void*, void*, void*, void**);
    void* archive = objc_msg_cast<NewArchiveFn>(state.objc.msg_send)(
        device,
        state.objc.sel_register_name("newBinaryArchiveWithDescriptor:error:"),
        desc,
        &error);
    release_objc_object(state, desc);
    if (archive == nullptr) return nullptr;

    using AllocFn = void* (*)(void*, void*);
    using InitCapacityFn = void* (*)(void*, void*, unsigned long);
    void* allocated = objc_msg_cast<AllocFn>(state.objc.msg_send)(
        array_cls, state.objc.sel_register_name("alloc"));
    void* array = objc_msg_cast<InitCapacityFn>(state.objc.msg_send)(
        allocated,
        state.objc.sel_register_name("initWithCapacity:"),
        1ul);
    if (array == nullptr) {
        release_objc_object(state, archive);
        return nullptr;
    }
    using AddObjectFn = void (*)(void*, void*, void*);
    objc_msg_cast<AddObjectFn>(state.objc.msg_send)(
        array, state.objc.sel_register_name("addObject:"), archive);
    release_objc_object(state, archive);
    return array;
}

void fail_active_metal_archive_scope(
    MetalPipelineArchiveInterpose& state,
    ActiveMetalArchivePipelineScope& scope,
    std::string reason) noexcept {
    scope.audit.sticky_failure = true;
    try {
        if (scope.failure.empty()) scope.failure = reason;
    } catch (...) {
    }
    mark_metal_archive_sticky_failure(state, std::move(reason));
}

enum class MetalPipelineInterceptVerdict : std::uint8_t {
    outside_scope,
    enforced,
    rejected,
};

[[nodiscard]] MetalPipelineInterceptVerdict prepare_scoped_metal_pipeline_call(
    void* self,
    void* desc,
    unsigned long options,
    bool asynchronous) noexcept {
    auto& state = metal_pipeline_archive_interpose_state();
    try {
    std::unique_lock<std::mutex> active_guard(state.active_scope_mu);
    const std::shared_ptr<ActiveMetalArchivePipelineScope> scope =
        state.active_scope;
    if (scope == nullptr) {
        state.outside_scope_interceptions.fetch_add(
            1u, std::memory_order_relaxed);
        active_guard.unlock();
        if (state.installed) {
            mark_metal_archive_sticky_failure(
                state,
                "immutable Metal runtime intercepted an unscoped pipeline creation");
            return MetalPipelineInterceptVerdict::rejected;
        }
        return MetalPipelineInterceptVerdict::outside_scope;
    }
    std::unique_lock<std::mutex> scope_guard(scope->mu);
    active_guard.unlock();
    if (!scope->active || scope->finished) {
        state.outside_scope_interceptions.fetch_add(
            1u, std::memory_order_relaxed);
        scope_guard.unlock();
        if (state.installed) {
            mark_metal_archive_sticky_failure(
                state,
                "immutable Metal runtime intercepted a pipeline outside the live receipt scope");
            return MetalPipelineInterceptVerdict::rejected;
        }
        return MetalPipelineInterceptVerdict::outside_scope;
    }
    try {
        ++scope->audit.scoped_interceptions;
        if (scope->audit.scoped_interceptions != 1u) {
            fail_active_metal_archive_scope(
                state, *scope,
                "one Vulkan createInfo unexpectedly produced more than one Metal PSO interception");
            return MetalPipelineInterceptVerdict::rejected;
        }
        const std::string observed_entry =
            descriptor_function_name(state, desc);
        if (observed_entry.empty()
            || observed_entry != scope->audit.expected_entry_name) {
            fail_active_metal_archive_scope(
                state, *scope,
                fmt::format(
                    "intercepted Metal function '{}' differs from expected '{}'",
                    observed_entry,
                    scope->audit.expected_entry_name));
            return MetalPipelineInterceptVerdict::rejected;
        }
        scope->audit.observed_entry_name = observed_entry;
        ++scope->audit.matched_descriptor_functions;
        if (state.sticky_failure.load(std::memory_order_acquire)) {
            fail_active_metal_archive_scope(
                state, *scope, metal_archive_failure_reason(state));
            return MetalPipelineInterceptVerdict::rejected;
        }
        if (self == nullptr || desc == nullptr
            || state.archive_array == nullptr
            || state.original_new_pipeline_sync == nullptr) {
            fail_active_metal_archive_scope(
                state, *scope,
                "Metal archive interpose lacks descriptor, archive set, or original sync IMP");
            return MetalPipelineInterceptVerdict::rejected;
        }
        const std::uint64_t intercepted_registry =
            metal_device_registry_id(state, self);
        if (intercepted_registry == 0u
            || intercepted_registry
                != state.runtime_metal_registry_id) {
            fail_active_metal_archive_scope(
                state, *scope,
                fmt::format(
                    "intercepted Metal registry id {} differs from sealed {}",
                    intercepted_registry,
                    state.runtime_metal_registry_id));
            return MetalPipelineInterceptVerdict::rejected;
        }
        if (scope->audit.intercepted_metal_registry_id != 0u
            && scope->audit.intercepted_metal_registry_id
                != intercepted_registry) {
            fail_active_metal_archive_scope(
                state, *scope,
                "one pipeline scope intercepted more than one Metal device");
            return MetalPipelineInterceptVerdict::rejected;
        }
        scope->audit.intercepted_metal_registry_id = intercepted_registry;
        auto& command_state = moltenvk_command_interpose_state();
        const CommandInterposeDeviceBinding command_binding =
            command_interpose_device_binding(self);
        scope->audit.command_source_methods_present =
            command_binding.methods_present;
        scope->audit.command_source_wrapper_methods =
            command_binding.wrapper_methods;
        if (!command_state.cached_binding_validated
            || command_state.loader_registry_id
                != scope->audit.command_loader_metal_registry_id
            || command_binding.registry_id != intercepted_registry
            || command_binding.registry_id
                != command_state.loader_registry_id
            || command_binding.methods_present != 2u
            || command_binding.wrapper_methods
                != command_binding.methods_present) {
            fail_active_metal_archive_scope(
                state, *scope,
                "intercepted Metal device does not retain both immutable command-source wrappers");
            return MetalPipelineInterceptVerdict::rejected;
        }

        using CountFn = unsigned long (*)(void*, void*);
        const unsigned long exact_count =
            objc_msg_cast<CountFn>(state.objc.msg_send)(
                state.archive_array, state.sel_count);
        const std::size_t configured_archive_count =
            state.configuration_image
                ? state.configuration_image->configuration().archives.size()
                : 0u;
        if (exact_count != configured_archive_count) {
            fail_active_metal_archive_scope(
                state, *scope,
                "resident Metal archive object count differs from sealed set");
            return MetalPipelineInterceptVerdict::rejected;
        }

        void* empty_array = make_empty_metal_archive_array(state, self);
        if (empty_array == nullptr) {
            fail_active_metal_archive_scope(
                state, *scope,
                "cannot construct same-device empty archive negative control");
            return MetalPipelineInterceptVerdict::rejected;
        }
        using SetObjFn = void (*)(void*, void*, void*);
        objc_msg_cast<SetObjFn>(state.objc.msg_send)(
            desc, state.sel_set_binary_archives, empty_array);
        ++scope->audit.same_descriptor_negative_controls;

        void* negative_reflection = nullptr;
        void* negative_error = nullptr;
        using SyncFn = void* (*)(
            void*, void*, void*, unsigned long, void**, void**);
        ++scope->audit.original_sync_negative_calls;
        void* unexpected_pipeline = objc_msg_cast<SyncFn>(
            state.original_new_pipeline_sync)(
                self,
                state.sel_new_pipeline_sync,
                desc,
                options | kMetalPipelineFailOnBinaryArchiveMiss,
                &negative_reflection,
                &negative_error);
        if (unexpected_pipeline != nullptr || negative_error == nullptr) {
            release_objc_object(state, unexpected_pipeline);
            release_objc_object(state, empty_array);
            fail_active_metal_archive_scope(
                state, *scope,
                "empty archive negative control did not produce a binary-archive miss");
            return MetalPipelineInterceptVerdict::rejected;
        }
        const MetalErrorIdentity negative_identity =
            metal_error_identity(state, negative_error);
        scope->audit.observed_negative_error_domain =
            negative_identity.domain;
        scope->audit.observed_negative_error_code = negative_identity.code;
        if (negative_identity.domain
                != scope->audit.expected_negative_error_domain
            || negative_identity.code
                != scope->audit.expected_negative_error_code) {
            release_objc_object(state, empty_array);
            fail_active_metal_archive_scope(
                state, *scope,
                fmt::format(
                    "empty archive negative control returned NSError ({}, {}) instead of sealed ({}, {})",
                    negative_identity.domain,
                    negative_identity.code,
                    scope->audit.expected_negative_error_domain,
                    scope->audit.expected_negative_error_code));
            return MetalPipelineInterceptVerdict::rejected;
        }
        ++scope->audit.negative_control_misses;

        // This replacement is unconditional: a descriptor-provided array can
        // neither augment nor override the exact manifest-owned set.
        objc_msg_cast<SetObjFn>(state.objc.msg_send)(
            desc, state.sel_set_binary_archives, state.archive_array);
        release_objc_object(state, empty_array);
        if (!state.process_qualification.accepted()
            || state.process_qualification
                != scope->audit.cache_qualification) {
            fail_active_metal_archive_scope(
                state, *scope,
                "process Metal archive qualification changed before pipeline");
            return MetalPipelineInterceptVerdict::rejected;
        }
        scope->audit.pipeline_pre_identity_checks = 0u;
        ++scope->audit.descriptor_replacements;
        ++scope->audit.fail_on_miss_enforcements;
        if (asynchronous) {
            ++scope->audit.actual_async_calls;
        } else {
            ++scope->audit.actual_sync_calls;
        }
        std::fprintf(
            stderr,
            "[bioseq-gpu-diag] pso-interpose: enforced immutable archives count=%lu function=%s registry=%llu\n",
            exact_count,
            descriptor_function_name(state, desc).c_str(),
            static_cast<unsigned long long>(intercepted_registry));
        return MetalPipelineInterceptVerdict::enforced;
    } catch (const std::exception& error) {
        fail_active_metal_archive_scope(state, *scope, error.what());
        return MetalPipelineInterceptVerdict::rejected;
    } catch (...) {
        fail_active_metal_archive_scope(
            state, *scope, "unknown immutable Metal archive interpose failure");
        return MetalPipelineInterceptVerdict::rejected;
    }
    } catch (...) {
        mark_metal_archive_sticky_failure_literal(
            state,
            "immutable Metal archive interpose failed before scoped handling");
        return MetalPipelineInterceptVerdict::rejected;
    }
}

extern "C" void bioseq_metal_new_compute_pipeline_state_async(
    void* self,
    void* cmd,
    void* desc,
    unsigned long options,
    void* completion_handler)
{
    auto& state = metal_pipeline_archive_interpose_state();
    using AsyncFn = void (*)(void*, void*, void*, unsigned long, void*);
    const MetalPipelineInterceptVerdict verdict =
        prepare_scoped_metal_pipeline_call(
            self, desc, options, true);
    if (verdict == MetalPipelineInterceptVerdict::outside_scope) {
        objc_msg_cast<AsyncFn>(state.original_new_pipeline_async)(
            self, cmd, desc, options, completion_handler);
        return;
    }
    if (verdict == MetalPipelineInterceptVerdict::rejected) {
        invoke_metal_pipeline_completion(
            completion_handler, nullptr, nullptr, nullptr);
        return;
    }
    objc_msg_cast<AsyncFn>(state.original_new_pipeline_async)(
        self,
        cmd,
        desc,
        options | kMetalPipelineFailOnBinaryArchiveMiss,
        completion_handler);
}

extern "C" void* bioseq_metal_new_compute_pipeline_state_sync(
    void* self,
    void* cmd,
    void* desc,
    unsigned long options,
    void** reflection,
    void** error)
{
    auto& state = metal_pipeline_archive_interpose_state();
    using SyncFn = void* (*)(void*, void*, void*, unsigned long, void**, void**);
    const MetalPipelineInterceptVerdict verdict =
        prepare_scoped_metal_pipeline_call(
            self, desc, options, false);
    if (verdict == MetalPipelineInterceptVerdict::outside_scope) {
        return objc_msg_cast<SyncFn>(state.original_new_pipeline_sync)(
            self, cmd, desc, options, reflection, error);
    }
    if (verdict == MetalPipelineInterceptVerdict::rejected) {
        return nullptr;
    }
    return objc_msg_cast<SyncFn>(state.original_new_pipeline_sync)(
        self,
        cmd,
        desc,
        options | kMetalPipelineFailOnBinaryArchiveMiss,
        reflection,
        error);
}

void install_metal_pipeline_archive_interpose(
    RuntimeVulkanDeviceIdentity selected_vulkan_device) noexcept {
    auto& state = metal_pipeline_archive_interpose_state();
    void* async_method = nullptr;
    void* sync_method = nullptr;
    const auto rollback_install = [&state]() noexcept {
        rollback_metal_pipeline_archive_interpose_install(state);
    };
    try {
    if (state.sticky_failure.load(std::memory_order_acquire)) {
        return;
    }
    if (state.installed) {
        return;
    }
    const auto configuration = runtime_metal_archive_configuration_snapshot();
    if (!configuration) {
        std::fprintf(
            stderr,
            "[bioseq-gpu-diag] pso-interpose: no sealed runtime archive configuration; interpose inactive\n");
        return;
    }
    if (!configuration->accepted()) {
        mark_metal_archive_sticky_failure(
            state, "installed runtime Metal archive configuration is invalid");
        return;
    }

    void* objc_handle = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
    void* metal_handle =
        ::dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
    if (objc_handle == nullptr || metal_handle == nullptr) {
        mark_metal_archive_sticky_failure(
            state, "Objective-C or Metal runtime cannot be loaded");
        return;
    }

    ObjcRuntimeFns objc{};
    objc.msg_send = ::dlsym(objc_handle, "objc_msgSend");
    objc.get_class = reinterpret_cast<decltype(objc.get_class)>(
        ::dlsym(objc_handle, "objc_getClass"));
    objc.sel_register_name = reinterpret_cast<decltype(objc.sel_register_name)>(
        ::dlsym(objc_handle, "sel_registerName"));
    objc.object_get_class = reinterpret_cast<decltype(objc.object_get_class)>(
        ::dlsym(objc_handle, "object_getClass"));
    objc.class_get_superclass = reinterpret_cast<decltype(objc.class_get_superclass)>(
        ::dlsym(objc_handle, "class_getSuperclass"));
    objc.class_get_instance_method = reinterpret_cast<decltype(objc.class_get_instance_method)>(
        ::dlsym(objc_handle, "class_getInstanceMethod"));
    objc.method_get_implementation = reinterpret_cast<decltype(objc.method_get_implementation)>(
        ::dlsym(objc_handle, "method_getImplementation"));
    objc.method_set_implementation = reinterpret_cast<decltype(objc.method_set_implementation)>(
        ::dlsym(objc_handle, "method_setImplementation"));
    if (objc.msg_send == nullptr || objc.get_class == nullptr
        || objc.sel_register_name == nullptr || objc.object_get_class == nullptr
        || objc.class_get_superclass == nullptr || objc.class_get_instance_method == nullptr
        || objc.method_get_implementation == nullptr
        || objc.method_set_implementation == nullptr) {
        mark_metal_archive_sticky_failure(
            state, "Objective-C runtime lacks required interpose symbols");
        return;
    }

    using CreateDeviceFn = void* (*)();
    auto create_device = reinterpret_cast<CreateDeviceFn>(
        ::dlsym(metal_handle, "MTLCreateSystemDefaultDevice"));
    if (create_device == nullptr) {
        mark_metal_archive_sticky_failure(
            state, "MTLCreateSystemDefaultDevice is unavailable");
        return;
    }
    void* device = create_device();
    if (device == nullptr) {
        mark_metal_archive_sticky_failure(
            state, "MTLCreateSystemDefaultDevice returned null");
        return;
    }
    void* release_selector = objc.sel_register_name("release");
    ScopedObjcRelease device_owner{
        &objc, release_selector, device};

    void* cls = objc.object_get_class(device);
    void* sel_async =
        objc.sel_register_name("newComputePipelineStateWithDescriptor:options:completionHandler:");
    void* sel_sync =
        objc.sel_register_name("newComputePipelineStateWithDescriptor:options:reflection:error:");
    for (void* cursor = cls; cursor != nullptr && (async_method == nullptr || sync_method == nullptr);
         cursor = objc.class_get_superclass(cursor)) {
        if (async_method == nullptr) {
            async_method = objc.class_get_instance_method(cursor, sel_async);
        }
        if (sync_method == nullptr) {
            sync_method = objc.class_get_instance_method(cursor, sel_sync);
        }
    }
    if (sync_method == nullptr) {
        mark_metal_archive_sticky_failure(
            state,
            "original synchronous Metal pipeline IMP is required for negative control");
        return;
    }
    std::fprintf(stderr, "[bioseq-gpu-diag] pso-interpose: methods found async=%d sync=%d\n",
                 async_method != nullptr ? 1 : 0, sync_method != nullptr ? 1 : 0);

    state.objc = objc;
    state.configuration_image = configuration;
    const RuntimeMetalArchiveConfiguration& installed_configuration =
        state.configuration_image->configuration();
    state.configured_identity = runtime_metal_archive_set_identity(
        installed_configuration.archives);
    state.configuration_validated = state.configured_identity.accepted();
    if (!state.configuration_validated) {
        rollback_install();
        mark_metal_archive_sticky_failure(
            state, "runtime Metal archive configuration lost install-time validation");
        return;
    }
    state.sel_new_pipeline_sync = sel_sync;
    state.sel_set_binary_archives = objc.sel_register_name("setBinaryArchives:");
    state.sel_compute_function = objc.sel_register_name("computeFunction");
    state.sel_name = objc.sel_register_name("name");
    state.sel_utf8_string = objc.sel_register_name("UTF8String");
    state.sel_error_domain = objc.sel_register_name("domain");
    state.sel_error_code = objc.sel_register_name("code");
    state.sel_count = objc.sel_register_name("count");
    state.sel_registry_id = objc.sel_register_name("registryID");
    state.sel_release = release_selector;
    state.sel_max_total_threads = objc.sel_register_name("maxTotalThreadsPerThreadgroup");
    state.sel_threadgroup_multiple =
        objc.sel_register_name("threadGroupSizeIsMultipleOfThreadExecutionWidth");

    const bool selected_uuid_missing = std::all_of(
        selected_vulkan_device.device_uuid.begin(),
        selected_vulkan_device.device_uuid.end(),
        [](std::uint8_t byte) { return byte == 0u; });
    const bool configured_uuid_present = std::any_of(
        installed_configuration.vulkan_device_uuid.begin(),
        installed_configuration.vulkan_device_uuid.end(),
        [](std::uint8_t byte) { return byte != 0u; });
    if (!selected_vulkan_device.accepted() || selected_uuid_missing
        || (configured_uuid_present
            && installed_configuration.vulkan_device_uuid
                != selected_vulkan_device.device_uuid)) {
        rollback_install();
        mark_metal_archive_sticky_failure(
            state,
            "selected Vulkan device UUID differs from optional sealed expectation");
        return;
    }
    state.runtime_vulkan_device_uuid = selected_vulkan_device.device_uuid;
    state.runtime_vulkan_device_identity = selected_vulkan_device;

    state.loader_registry_id = metal_device_registry_id(state, device);
    if (state.loader_registry_id == 0u
        || state.loader_registry_id
            != installed_configuration.helper_metal_registry_id
        || (installed_configuration.metal_device_registry_id != 0u
            && state.loader_registry_id
                != installed_configuration.metal_device_registry_id)) {
        rollback_install();
        mark_metal_archive_sticky_failure(
            state,
            fmt::format(
                "Metal archive loader registry id {} differs from helper-qualified {} or optional sealed {}",
                state.loader_registry_id,
                installed_configuration.helper_metal_registry_id,
                installed_configuration.metal_device_registry_id));
        return;
    }
    state.runtime_metal_registry_id = state.loader_registry_id;
    if (!load_exact_metal_binary_archives(state, device)) {
        rollback_install();
        return;
    }

    state.interposed_new_pipeline_async_method = async_method;
    state.interposed_new_pipeline_sync_method = sync_method;
    if (async_method != nullptr) {
        state.original_new_pipeline_async =
            objc.method_set_implementation(
                async_method,
                reinterpret_cast<void*>(&bioseq_metal_new_compute_pipeline_state_async));
    }
    if (sync_method != nullptr) {
        state.original_new_pipeline_sync =
            objc.method_set_implementation(
                sync_method,
                reinterpret_cast<void*>(&bioseq_metal_new_compute_pipeline_state_sync));
    }
    state.installed = state.original_new_pipeline_sync != nullptr
        && (async_method == nullptr
            || state.original_new_pipeline_async != nullptr);
    std::fprintf(stderr, "[bioseq-gpu-diag] pso-interpose: installed=%d\n", state.installed ? 1 : 0);
    if (!state.installed) {
        rollback_install();
        mark_metal_archive_sticky_failure(
            state, "Metal pipeline method interposition did not install exactly");
        return;
    }
    state.loader_device = device_owner.release();
    try {
        bioseq_log<DBG>(
            "[vulkan_runtime] installed immutable Metal archive interpose manifest={} registry={} archives={} async={} sync={}",
            installed_configuration.seed_image->manifest().manifest_checksum,
            state.loader_registry_id,
            state.loaded_identity.archive_count,
            state.original_new_pipeline_async != nullptr,
            state.original_new_pipeline_sync != nullptr);
    } catch (...) {
        // Diagnostics cannot invalidate a fully committed interpose.
    }
    } catch (const std::bad_alloc&) {
        rollback_install();
        mark_metal_archive_sticky_failure_literal(
            state, "immutable Metal archive install allocation failed");
    } catch (...) {
        rollback_install();
        mark_metal_archive_sticky_failure_literal(
            state, "immutable Metal archive install threw unexpectedly");
    }
}

class MetalArchivePipelineCreateScope {
public:
    MetalArchivePipelineCreateScope(
        bool required,
        std::array<std::uint8_t, VK_UUID_SIZE> runtime_device_uuid,
        ShaderVariantKey shader_key,
        std::string expected_entry_name)
        : required_(required) {
        auto& state = metal_pipeline_archive_interpose_state();
        if (!runtime_metal_archive_configuration_installed()
            || !state.configuration_image) {
            if (required_) {
                failure_ =
                    "provider-backed Vulkan pipeline has no sealed Metal archive configuration";
            }
            return;
        }
        const RuntimeMetalArchiveConfiguration& configuration =
            state.configuration_image->configuration();
        const SealedShaderCacheManifest& manifest =
            configuration.seed_image->manifest();
        const SealedShaderCacheSeedReceipt& seed_receipt =
            configuration.seed_image->receipt();
        audit_.cache_digest = manifest.cache_digest;
        audit_.seed_sha256 = manifest.seed_sha256;
        audit_.seed_size = manifest.seed_size;
        audit_.manifest_checksum = manifest.manifest_checksum;
        audit_.seed_receipt_identity = seed_receipt.identity();
        audit_.seed_load_count = seed_receipt.load_count();
        audit_.seed_manifest_read_count =
            seed_receipt.manifest_read_count();
        audit_.seed_artifact_read_count =
            seed_receipt.artifact_read_count();
        audit_.seed_manifest_bytes = seed_receipt.manifest_bytes();
        audit_.seed_artifact_bytes = seed_receipt.artifact_bytes();
        audit_.seed_load_ns = seed_receipt.load_ns();
        audit_.seed_admission_count = seed_receipt.admission_count();
        audit_.seed_peak_admitted_bytes =
            seed_receipt.peak_admitted_bytes();
        audit_.seed_retained_capacity_bytes =
            seed_receipt.retained_capacity_bytes();
        audit_.seed_admission_saturated =
            seed_receipt.admission_saturated();
        audit_.cache_qualification = state.process_qualification;
        audit_.shader_key = shader_key;
        audit_.expected_entry_name = std::move(expected_entry_name);
        audit_.expected_archives =
            runtime_metal_archive_set_identity(configuration.archives);
        audit_.loaded_archives = state.loaded_identity;
        audit_.expected_negative_error_domain =
            configuration.archives.front().negative_error_domain;
        audit_.expected_negative_error_code =
            configuration.archives.front().negative_error_code;
        audit_.archive_pre_post_identity_checks =
            state.loaded_pre_post_identity_checks;
        auto& command_state = moltenvk_command_interpose_state();
        audit_.command_interpose_installed = command_state.installed;
        audit_.command_interpose_immutable_fail_closed =
            command_state.immutable_fail_closed;
        audit_.command_loader_metal_registry_id =
            command_state.loader_registry_id;
        audit_.unsealed_metal_source_requests =
            command_state.recognized_source_requests.load(
                std::memory_order_relaxed);
        audit_.metal_source_fallback_calls =
            command_state.source_fallback_calls.load(
                std::memory_order_relaxed);
        audit_.configured_vulkan_device_uuid =
            state.runtime_vulkan_device_uuid;
        audit_.runtime_vulkan_device_uuid = runtime_device_uuid;
        audit_.expected_metal_registry_id =
            state.runtime_metal_registry_id;
        audit_.loader_metal_registry_id = state.loader_registry_id;
        audit_.scope_identity = next_nonzero_identity(
            state.next_scope_identity);
        if (audit_.scope_identity == 0u) {
            failure_ = "Metal archive pipeline scope identity space exhausted";
            return;
        }
        if (!state.installed || state.archive_array == nullptr
            || !audit_.cache_qualification.accepted()) {
            failure_ = "sealed Metal archive interpose is not installed";
            return;
        }
        if (state.sticky_failure.load(std::memory_order_acquire)) {
            audit_.sticky_failure = true;
            failure_ = metal_archive_failure_reason(state);
            return;
        }
        if (!audit_.command_interpose_installed
            || !audit_.command_interpose_immutable_fail_closed) {
            failure_ =
                "immutable MoltenVK command-source interpose is not installed fail-closed";
            mark_metal_archive_sticky_failure(state, failure_);
            return;
        }
        if (command_state.sticky_failure.load(std::memory_order_acquire)
            || audit_.unsealed_metal_source_requests != 0u
            || audit_.metal_source_fallback_calls != 0u) {
            failure_ =
                "immutable GPU runtime observed an unsealed Metal source-library request";
            mark_metal_archive_sticky_failure(state, failure_);
            return;
        }
        if (audit_.configured_vulkan_device_uuid
            != audit_.runtime_vulkan_device_uuid) {
            failure_ = "runtime Vulkan UUID differs from sealed archive UUID";
            mark_metal_archive_sticky_failure(state, failure_);
            return;
        }
        active_ = std::make_shared<ActiveMetalArchivePipelineScope>();
        active_->audit = audit_;
        active_->required = required_;
        active_->active = true;
        {
            std::lock_guard<std::mutex> guard(state.active_scope_mu);
            if (state.active_scope != nullptr) {
                audit_.nested_scope = true;
                failure_ = "nested Metal archive pipeline-create scope rejected";
                mark_metal_archive_sticky_failure(state, failure_);
                active_.reset();
                return;
            }
            active_->outside_scope_interceptions_at_begin =
                state.outside_scope_interceptions.load(
                    std::memory_order_relaxed);
            state.active_scope = active_;
        }
        active_scope_ = true;
    }

    MetalArchivePipelineCreateScope(
        const MetalArchivePipelineCreateScope&) = delete;
    MetalArchivePipelineCreateScope& operator=(
        const MetalArchivePipelineCreateScope&) = delete;

    ~MetalArchivePipelineCreateScope() noexcept {
        if (!active_scope_ || active_ == nullptr) return;
        auto& state = metal_pipeline_archive_interpose_state();
        try {
            std::unique_lock<std::mutex> active_guard(state.active_scope_mu);
            try {
                std::unique_lock<std::mutex> scope_guard(active_->mu);
                if (state.active_scope == active_) state.active_scope.reset();
                active_->active = false;
                active_->finished = true;
                if (!finished_) {
                    mark_metal_archive_sticky_failure_literal(
                        state,
                        "Metal archive pipeline-create scope abandoned before verdict");
                }
            } catch (...) {
                if (state.active_scope == active_) state.active_scope.reset();
                mark_metal_archive_sticky_failure_literal(
                    state,
                    "Metal archive pipeline-create scope cleanup failed");
            }
        } catch (...) {
            mark_metal_archive_sticky_failure_literal(
                state,
                "Metal archive pipeline-create scope cleanup failed");
        }
    }

    [[nodiscard]] bool ready_for_pipeline_call() const noexcept {
        return failure_.empty() && (active_scope_ || !required_);
    }

    [[nodiscard]] std::expected<
        std::optional<RuntimeMetalArchivePipelineAudit>, std::string>
    finish(VkResult outer_result) {
        if (finished_) {
            return std::unexpected(
                "Metal archive pipeline-create scope verdict consumed twice");
        }
        finished_ = true;
        if (!failure_.empty()) return std::unexpected(failure_);
        if (!active_scope_) {
            if (required_) {
                return std::unexpected(
                    "provider-backed Vulkan pipeline did not enter immutable Metal archive scope");
            }
            return std::optional<RuntimeMetalArchivePipelineAudit>{};
        }
        auto& state = metal_pipeline_archive_interpose_state();
        {
            std::unique_lock<std::mutex> active_guard(state.active_scope_mu);
            std::unique_lock<std::mutex> scope_guard(active_->mu);
            if (state.active_scope != active_) {
                return std::unexpected(
                    "Metal archive pipeline-create scope ownership changed before verdict");
            }
            const std::uint64_t outside_end =
                state.outside_scope_interceptions.load(
                    std::memory_order_relaxed);
            if (outside_end < active_->outside_scope_interceptions_at_begin) {
                active_->audit.outside_scope_interceptions =
                    (std::numeric_limits<std::uint64_t>::max)();
            } else {
                active_->audit.outside_scope_interceptions =
                    outside_end
                    - active_->outside_scope_interceptions_at_begin;
            }
            active_->active = false;
            active_->finished = true;
            state.active_scope.reset();
            audit_ = std::move(active_->audit);
            if (!active_->failure.empty()) failure_ = active_->failure;
        }
        if (!failure_.empty()) return std::unexpected(failure_);
        if (!state.process_qualification.accepted()
            || state.process_qualification != audit_.cache_qualification) {
            failure_ =
                "process Metal archive qualification changed after pipeline";
            mark_metal_archive_sticky_failure(state, failure_);
            return std::unexpected(failure_);
        }
        audit_.pipeline_post_identity_checks = 0u;
        auto& command_state = moltenvk_command_interpose_state();
        audit_.unsealed_metal_source_requests =
            command_state.recognized_source_requests.load(
                std::memory_order_relaxed);
        audit_.metal_source_fallback_calls =
            command_state.source_fallback_calls.load(
                std::memory_order_relaxed);
        audit_.command_interpose_installed = command_state.installed;
        audit_.command_interpose_immutable_fail_closed =
            command_state.immutable_fail_closed;
        audit_.sticky_failure =
            audit_.sticky_failure
            || state.sticky_failure.load(std::memory_order_acquire);
        audit_.sticky_failure = audit_.sticky_failure
            || command_state.sticky_failure.load(
                std::memory_order_acquire);
        audit_.outer_vulkan_pipeline_succeeded = outer_result == VK_SUCCESS;
        audit_.receipt_identity = next_nonzero_identity(
            state.next_receipt_identity);
        if (!audit_.accepted()) {
            return std::unexpected(fmt::format(
                "immutable Metal archive receipt rejected: vk={} calls={} replace={} enforce={} negative={}/{} sticky={}",
                static_cast<int>(outer_result),
                audit_.scoped_interceptions,
                audit_.descriptor_replacements,
                audit_.fail_on_miss_enforcements,
                audit_.negative_control_misses,
                audit_.same_descriptor_negative_controls,
                audit_.sticky_failure));
        }
        return std::optional<RuntimeMetalArchivePipelineAudit>{
            std::move(audit_)};
    }

private:
    bool required_ = false;
    bool active_scope_ = false;
    bool finished_ = false;
    std::string failure_;
    RuntimeMetalArchivePipelineAudit audit_{};
    std::shared_ptr<ActiveMetalArchivePipelineScope> active_{};
};
#else
void install_moltenvk_command_library_interpose() noexcept {}
void install_metal_pipeline_archive_interpose(
    RuntimeVulkanDeviceIdentity) noexcept {}

class MetalArchivePipelineCreateScope {
public:
    MetalArchivePipelineCreateScope(
        bool required,
        std::array<std::uint8_t, VK_UUID_SIZE>,
        ShaderVariantKey,
        std::string) noexcept
        : required_(required || runtime_metal_archive_configuration_installed()) {}

    [[nodiscard]] bool ready_for_pipeline_call() const noexcept {
        return !required_;
    }

    [[nodiscard]] std::expected<
        std::optional<RuntimeMetalArchivePipelineAudit>, std::string>
    finish(VkResult) const {
        if (required_) {
            return std::unexpected(
                "immutable Metal archive receipt requested on a non-Apple runtime");
        }
        return std::optional<RuntimeMetalArchivePipelineAudit>{};
    }

private:
    bool required_ = false;
};
#endif

void require_immutable_metal_policy_live(std::string_view phase) {
#if defined(__APPLE__)
    runtime_metal_archive_configuration_slot().live_gate_calls.fetch_add(
        1u, std::memory_order_relaxed);
    if (!runtime_metal_archive_configuration_installed()) return;
    auto& archive_state = metal_pipeline_archive_interpose_state();
    auto& command_state = moltenvk_command_interpose_state();
    const bool clean = archive_state.configuration_validated
        && archive_state.configured_identity.accepted()
        && archive_state.configured_identity == archive_state.loaded_identity
        && archive_state.installed
        && archive_state.archive_array != nullptr
        && archive_state.process_qualification.accepted()
        && archive_state.process_qualification.archive_set
            == archive_state.loaded_identity
        && !archive_state.sticky_failure.load(std::memory_order_acquire)
        && archive_state.outside_scope_interceptions.load(
               std::memory_order_relaxed) == 0u
        && command_state.installed
        && command_state.immutable_fail_closed
        && command_state.cached_binding_validated
        && !command_state.sticky_failure.load(std::memory_order_acquire)
        && command_state.recognized_source_requests.load(
               std::memory_order_relaxed) == 0u
        && command_state.source_fallback_calls.load(
               std::memory_order_relaxed) == 0u
        && archive_state.loader_registry_id
            == archive_state.runtime_metal_registry_id
        && archive_state.loader_registry_id
            == archive_state.process_qualification.helper_metal_registry_id
        && archive_state.runtime_vulkan_device_identity.device_uuid
            == archive_state.process_qualification.vulkan_device_uuid
        && archive_state.runtime_vulkan_device_identity.api_version
            == archive_state.process_qualification.api_version
        && archive_state.runtime_vulkan_device_identity.driver_version
            == archive_state.process_qualification.driver_version
        && archive_state.runtime_vulkan_device_identity.vendor_id
            == archive_state.process_qualification.vendor_id
        && archive_state.runtime_vulkan_device_identity.device_id
            == archive_state.process_qualification.device_id
        && archive_state.runtime_vulkan_device_identity.pipeline_cache_uuid
            == archive_state.process_qualification.pipeline_cache_uuid
        && command_state.loader_registry_id == archive_state.loader_registry_id
        && command_state.cached_methods_present == 2u
        && command_state.cached_wrapper_methods
            == command_state.cached_methods_present;
    if (!clean) {
        throw VulkanRuntimeError(fmt::format(
            "{} observed stale or poisoned immutable Metal policy state",
            phase));
    }
#else
    (void)phase;
#endif
}

void load_vulkan_runtime_functions() {
    std::string missing;
#define BIOSEQ_LOAD_VULKAN_FUNCTION(name) \
    do { \
        if (name == nullptr) { \
            name = reinterpret_cast<PFN_##name>(::dlsym(RTLD_DEFAULT, #name)); \
        } \
        if (name == nullptr) { \
            if (!missing.empty()) missing += ", "; \
            missing += #name; \
        } \
    } while (false);
    BIOSEQ_VULKAN_RUNTIME_FUNCTIONS(BIOSEQ_LOAD_VULKAN_FUNCTION)
#undef BIOSEQ_LOAD_VULKAN_FUNCTION
    if (!missing.empty()) {
        throw VulkanRuntimeError(fmt::format(
            "Vulkan loader is missing required entry points: {}", missing));
    }
}

void ensure_vulkan_loader_loaded_for_runtime() {
    if (::dlsym(RTLD_DEFAULT, "vkCreateInstance") == nullptr) {
#if defined(__APPLE__)
    constexpr const char* candidates[] = {
        "@rpath/libvulkan.1.4.341.dylib",
        "@rpath/libvulkan.1.dylib",
        "@rpath/libvulkan.dylib",
        "/opt/homebrew/lib/libvulkan.dylib",
        "/usr/local/lib/libvulkan.dylib",
    };
#else
    constexpr const char* candidates[] = {
        "libvulkan.so.1",
        "libvulkan.so",
    };
#endif
    std::string errors;
    bool loaded = false;
    for (const char* path : candidates) {
        ::dlerror();
        if (void* handle = ::dlopen(path, RTLD_NOW | RTLD_GLOBAL); handle != nullptr) {
            (void)handle;
            loaded = true;
            break;
        }
        if (const char* err = ::dlerror(); err != nullptr) {
            if (!errors.empty()) errors += "; ";
            errors += path;
            errors += ": ";
            errors += err;
        }
    }
    if (!loaded && ::dlsym(RTLD_DEFAULT, "vkCreateInstance") == nullptr) {
        throw VulkanRuntimeError(fmt::format(
            "Vulkan loader unavailable for backend runtime initialization: {}",
            errors.empty() ? "no dlopen diagnostics" : errors));
    }
    }
    load_vulkan_runtime_functions();
}

[[nodiscard]] std::string symbol_image(const void* symbol) {
#if defined(__APPLE__)
    Dl_info info{};
    if (symbol != nullptr && ::dladdr(symbol, &info) != 0 && info.dli_fname != nullptr) {
        return info.dli_fname;
    }
#else
    (void)symbol;
#endif
    return {};
}

}  // namespace

void probe_immutable_metal_policy_live_for_test() {
    require_immutable_metal_policy_live("unit-test live gate");
}

[[nodiscard]] std::expected<RuntimePrivateArchiveSnapshotTestEvidence, std::string>
verify_runtime_private_archive_snapshot_for_test(
    std::string_view artifact_filename,
    std::span<const std::byte> bytes,
    const std::filesystem::path& temp_parent)
{
#if defined(__APPLE__)
    if (artifact_filename.empty() || bytes.empty()) {
        return std::unexpected("private archive snapshot test input is empty");
    }
    RuntimeMetalArchiveSpec expected{
        .artifact_ordinal = 0u,
        .artifact_filename = std::string(artifact_filename),
        .size = static_cast<std::uint64_t>(bytes.size()),
        .sha256 = sealed_shader_cache_detail::sha256(bytes),
        .negative_error_domain = "MTLLibraryErrorDomain",
        .negative_error_code = 6,
    };
    auto private_image = write_private_metal_archive_snapshot(
        artifact_filename, bytes, temp_parent);
    if (!private_image) return std::unexpected(private_image.error());
    auto snapshot = capture_metal_archive_file_snapshot(
        private_image->path(), expected, bytes);
    if (!snapshot) return std::unexpected(snapshot.error());
    const int fd = ::open(private_image->path().c_str(), O_WRONLY);
    if (fd < 0) {
        return std::unexpected("cannot open private snapshot for tamper test");
    }
    const std::byte changed = bytes.front() ^ std::byte{0x01};
    const bool wrote = ::pwrite(fd, &changed, sizeof(changed), 0) == 1
        && ::fsync(fd) == 0;
    (void)::close(fd);
    if (!wrote) {
        return std::unexpected("cannot mutate private snapshot for tamper test");
    }
    const auto rejected = capture_metal_archive_file_snapshot(
        private_image->path(), expected, bytes);
    return RuntimePrivateArchiveSnapshotTestEvidence{
        .snapshot_path = private_image->path(),
        .streamed_bytes = snapshot->streamed_bytes,
        .scratch_capacity_bytes = snapshot->scratch_capacity_bytes,
        .exact_seed_compared = snapshot->exact_seed_compared,
        .tamper_rejected = !rejected,
    };
#else
    (void)artifact_filename;
    (void)bytes;
    (void)temp_parent;
    return std::unexpected(
        "private Metal archive snapshots are Apple-only");
#endif
}

// ---------------------------------------------------------------------------
// Buffer cleanup
// ---------------------------------------------------------------------------

void Buffer::reset() noexcept {
    if (owner != nullptr && buffer != VK_NULL_HANDLE) {
        owner->destroy_buffer(*this);
    }
    owner = nullptr;
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    bytes = 0;
    allocation_bytes = 0;
    memory_properties = 0;
    process_footprint_reflected = false;
    mapped = nullptr;
}

// ---------------------------------------------------------------------------
// Runtime lifecycle
// ---------------------------------------------------------------------------

Runtime::Runtime() {
    try {
        init();
    } catch (const std::exception& e) {
        init_error_ = e.what();
        bioseq_log<DBG>("[vulkan_runtime] init failed: {}", e.what());
        shutdown();
        features_ = VulkanFeatureSet{};
    } catch (...) {
        init_error_ = "unknown Vulkan initialization failure";
        bioseq_log<DBG>("[vulkan_runtime] init failed (unknown)");
        shutdown();
        features_ = VulkanFeatureSet{};
    }
}

Runtime::~Runtime() {
    shutdown();
}

Runtime& Runtime::instance() {
    static Runtime runtime;
    return runtime;
}

void Runtime::resolve_moltenvk_private_api() noexcept {
    set_workgroup_size_mvk_ = nullptr;
#if defined(__APPLE__)
    auto resolve_from = [](void* handle) noexcept
        -> PFN_vkSetWorkgroupSizeMVK_Bioseq {
        if (handle == nullptr) return nullptr;
        return reinterpret_cast<PFN_vkSetWorkgroupSizeMVK_Bioseq>(
            ::dlsym(handle, "vkSetWorkgroupSizeMVK"));
    };

    if (auto* fn = resolve_from(RTLD_DEFAULT); fn != nullptr) {
        set_workgroup_size_mvk_ = fn;
        return;
    }
    constexpr const char* candidates[] = {
        "/opt/homebrew/lib/libMoltenVK.dylib",
        "/usr/local/lib/libMoltenVK.dylib",
    };
    for (const char* path : candidates) {
        void* handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (auto* fn = resolve_from(handle); fn != nullptr) {
            set_workgroup_size_mvk_ = fn;
            return;
        }
    }
#endif
}

bool Runtime::use_shader_provider_locked() const noexcept {
    RuntimeShaderProvider* provider = runtime_shader_provider_slot();
    return provider != nullptr && provider->load_module != nullptr;
}

void prepare_shader_provider_runtime() {
    RuntimeShaderProvider* provider = runtime_shader_provider_slot();
    if (provider == nullptr || provider->prepare_runtime == nullptr) {
        return;
    }
    auto prepared = provider->prepare_runtime();
    if (!prepared) {
        throw VulkanRuntimeError(fmt::format(
            "runtime shader provider prepare failed: {}", prepared.error()));
    }
}

void Runtime::init() {
    // --- Instance ---
    ensure_vulkan_loader_loaded_for_runtime();
    ensure_homebrew_moltenvk_icd_visible();
    prepare_shader_provider_runtime();
#if defined(__APPLE__)
    // MoltenVK can compile its internal command shaders while enumerating or
    // creating the device. In a forked PostgreSQL backend that source compile
    // path reaches MTLCompilerService over XPC and aborts, so install the
    // cached command-library interpose before the first Vulkan loader query that
    // may enter MoltenVK device setup.
    install_moltenvk_command_library_interpose();
    if (runtime_metal_archive_configuration_installed()) {
        auto& command_state = moltenvk_command_interpose_state();
        if (!command_state.installed
            || !command_state.immutable_fail_closed) {
            throw VulkanRuntimeError(
                "immutable GPU mode could not install fail-closed MoltenVK command-source interpose");
        }
    }
#endif
    const auto inst_exts = instance_extensions();
    std::vector<const char*> enabled_inst_exts;
    VkInstanceCreateFlags inst_flags = 0;
    if (has_extension(inst_exts, kPortabilityEnumeration)) {
        enabled_inst_exts.push_back(kPortabilityEnumeration);
        inst_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    if (has_extension(inst_exts, kGetPhysicalDeviceProperties2)) {
        enabled_inst_exts.push_back(kGetPhysicalDeviceProperties2);
    }

    VkBool32 mvk_use_metal_argument_buffers = VK_FALSE;
    VkLayerSettingEXT mvk_settings[] = {{
        "MoltenVK",
        "useMetalArgumentBuffers",
        VK_LAYER_SETTING_TYPE_BOOL32_EXT,
        1u,
        &mvk_use_metal_argument_buffers,
    }};
    VkLayerSettingsCreateInfoEXT layer_settings{};
    layer_settings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    layer_settings.settingCount = 1u;
    layer_settings.pSettings = mvk_settings;
#if defined(__APPLE__)
    const bool has_layer_settings = has_extension(inst_exts, kLayerSettings);
    if (has_layer_settings) {
        enabled_inst_exts.push_back(kLayerSettings);
    }
#endif

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "bioseq";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "bioseq-gpu";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo inst_info{};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.flags = inst_flags;
    inst_info.pApplicationInfo = &app_info;
    inst_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_inst_exts.size());
    inst_info.ppEnabledExtensionNames =
        enabled_inst_exts.empty() ? nullptr : enabled_inst_exts.data();
#if defined(__APPLE__)
    if (has_layer_settings) {
        inst_info.pNext = &layer_settings;
    }
#endif

    check_vk(vkCreateInstance(&inst_info, nullptr, &instance_), "vkCreateInstance");
#if defined(__APPLE__)
    if (runtime_metal_archive_configuration_installed()) {
        auto& command_state = moltenvk_command_interpose_state();
        if (command_state.sticky_failure.load(std::memory_order_acquire)
            || command_state.source_fallback_calls.load(
                std::memory_order_relaxed) != 0u) {
            throw VulkanRuntimeError(
                "immutable GPU mode required an unsealed MoltenVK command source");
        }
    }
#endif
    resolve_moltenvk_private_api();

    // --- Physical device selection (compute queue + shaderInt64) ---
    std::uint32_t device_count = 0;
    check_vk(vkEnumeratePhysicalDevices(instance_, &device_count, nullptr),
             "vkEnumeratePhysicalDevices(count)");
    if (device_count == 0) {
        throw VulkanRuntimeError("no Vulkan physical devices");
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    check_vk(vkEnumeratePhysicalDevices(instance_, &device_count, devices.data()),
             "vkEnumeratePhysicalDevices(list)");

    bool device_chosen = false;
    bool device_has_portability_subset = false;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceFeatures candidate_features{};
        vkGetPhysicalDeviceFeatures(candidate, &candidate_features);
        if (candidate_features.shaderInt64 != VK_TRUE) {
            continue;  // Phase A requires native 64-bit ints in the shader
        }

        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        if (family_count == 0) continue;
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        std::optional<std::uint32_t> compute_family;
        for (std::uint32_t f = 0; f < family_count; ++f) {
            if (families[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                compute_family = f;
                break;
            }
        }
        if (!compute_family) continue;

        physical_ = candidate;
        queue_family_ = *compute_family;
        features_.shader_int64 = true;
        device_has_portability_subset =
            has_extension(device_extensions(candidate), kPortabilitySubset);
        device_chosen = true;
        break;
    }
    if (!device_chosen) {
        throw VulkanRuntimeError("no Vulkan device with a compute queue + shaderInt64");
    }

    // --- Logical device (enable shaderInt64) ---
    std::vector<const char*> enabled_dev_exts;
    if (device_has_portability_subset) {
        enabled_dev_exts.push_back(kPortabilitySubset);
    }

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.shaderInt64 = VK_TRUE;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_dev_exts.size());
    device_info.ppEnabledExtensionNames =
        enabled_dev_exts.empty() ? nullptr : enabled_dev_exts.data();
    device_info.pEnabledFeatures = &enabled_features;

    // Seal the selected physical-device UUID before vkCreateDevice: MoltenVK
    // may create Metal compute pipelines during logical-device construction,
    // so the process-wide interpose must already be bound to this identity.
    VkPhysicalDeviceIDProperties selected_id_properties{};
    selected_id_properties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 selected_properties2{};
    selected_properties2.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    selected_properties2.pNext = &selected_id_properties;
    vkGetPhysicalDeviceProperties2(physical_, &selected_properties2);
    RuntimeVulkanDeviceIdentity selected_device_identity{
        .api_version = selected_properties2.properties.apiVersion,
        .driver_version = selected_properties2.properties.driverVersion,
        .vendor_id = selected_properties2.properties.vendorID,
        .device_id = selected_properties2.properties.deviceID,
    };
    std::copy(
        std::begin(selected_id_properties.deviceUUID),
        std::end(selected_id_properties.deviceUUID),
        selected_device_identity.device_uuid.begin());
    std::copy(
        std::begin(selected_properties2.properties.pipelineCacheUUID),
        std::end(selected_properties2.properties.pipelineCacheUUID),
        selected_device_identity.pipeline_cache_uuid.begin());
    features_.device_uuid = selected_device_identity.device_uuid;
    features_.api_version = selected_device_identity.api_version;
    features_.driver_version = selected_device_identity.driver_version;
    features_.vendor_id = selected_device_identity.vendor_id;
    features_.device_id = selected_device_identity.device_id;
    features_.pipeline_cache_uuid = selected_device_identity.pipeline_cache_uuid;
    install_metal_pipeline_archive_interpose(selected_device_identity);
    const VkResult create_device_result =
        vkCreateDevice(physical_, &device_info, nullptr, &device_);
#if defined(__APPLE__)
    if (create_device_result != VK_SUCCESS
        && runtime_metal_archive_configuration_installed()) {
        auto& archive_state = metal_pipeline_archive_interpose_state();
        mark_metal_archive_sticky_failure(
            archive_state,
            fmt::format(
                "vkCreateDevice failed after immutable Metal archive install ({})",
                static_cast<std::int32_t>(create_device_result)));
        rollback_metal_pipeline_archive_interpose_install(archive_state);
    }
    if (runtime_metal_archive_configuration_installed()) {
        auto& archive_state = metal_pipeline_archive_interpose_state();
        if (archive_state.sticky_failure.load(std::memory_order_acquire)
            || archive_state.outside_scope_interceptions.load(
                   std::memory_order_relaxed) != 0u) {
            if (!archive_state.sticky_failure.load(
                    std::memory_order_acquire)) {
                mark_metal_archive_sticky_failure_literal(
                    archive_state,
                    "Metal pipeline interception escaped its scope during vkCreateDevice");
            }
            const std::string failure = metal_archive_failure_reason(
                archive_state);
            if (create_device_result == VK_SUCCESS
                && device_ != VK_NULL_HANDLE) {
                vkDestroyDevice(device_, nullptr);
                device_ = VK_NULL_HANDLE;
            }
            rollback_metal_pipeline_archive_interpose_install(archive_state);
            throw VulkanRuntimeError(fmt::format(
                "immutable Metal archive policy failed during vkCreateDevice: {}",
                failure));
        }
    }
#endif
    check_vk(create_device_result, "vkCreateDevice");
    VkPhysicalDeviceIDProperties verified_id_properties{};
    verified_id_properties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 verified_properties2{};
    verified_properties2.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    verified_properties2.pNext = &verified_id_properties;
    vkGetPhysicalDeviceProperties2(physical_, &verified_properties2);
    RuntimeVulkanDeviceIdentity verified_device_identity{
        .api_version = verified_properties2.properties.apiVersion,
        .driver_version = verified_properties2.properties.driverVersion,
        .vendor_id = verified_properties2.properties.vendorID,
        .device_id = verified_properties2.properties.deviceID,
    };
    std::copy(
        std::begin(verified_id_properties.deviceUUID),
        std::end(verified_id_properties.deviceUUID),
        verified_device_identity.device_uuid.begin());
    std::copy(
        std::begin(verified_properties2.properties.pipelineCacheUUID),
        std::end(verified_properties2.properties.pipelineCacheUUID),
        verified_device_identity.pipeline_cache_uuid.begin());
    if (verified_device_identity != selected_device_identity) {
#if defined(__APPLE__)
        mark_metal_archive_sticky_failure(
            metal_pipeline_archive_interpose_state(),
            "Vulkan physical device identity changed across vkCreateDevice");
#endif
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
#if defined(__APPLE__)
        rollback_metal_pipeline_archive_interpose_install(
            metal_pipeline_archive_interpose_state());
#endif
        throw VulkanRuntimeError(
            "Vulkan physical device identity changed across vkCreateDevice");
    }
    try {
        require_immutable_metal_policy_live("vkCreateDevice completion");
    } catch (...) {
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
#if defined(__APPLE__)
        if (runtime_metal_archive_configuration_installed()) {
            auto& archive_state = metal_pipeline_archive_interpose_state();
            mark_metal_archive_sticky_failure_literal(
                archive_state,
                "immutable Metal policy final live gate failed after vkCreateDevice");
            rollback_metal_pipeline_archive_interpose_install(archive_state);
        }
#endif
        throw;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    metallib_provider_capable_ = set_workgroup_size_mvk_ != nullptr;

    // --- Limits + scalar-block-layout probe ---
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar_features{};
    scalar_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &scalar_features;
    if (has_extension(inst_exts, kGetPhysicalDeviceProperties2)) {
        vkGetPhysicalDeviceFeatures2(physical_, &features2);
        features_.scalar_block_layout = scalar_features.scalarBlockLayout == VK_TRUE;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    VkPhysicalDeviceIDProperties id_properties{};
    id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &id_properties;
    vkGetPhysicalDeviceProperties2(physical_, &properties2);
    std::copy(
        std::begin(id_properties.deviceUUID),
        std::end(id_properties.deviceUUID),
        features_.device_uuid.begin());
    features_.device_type = static_cast<std::uint32_t>(props.deviceType);
    features_.api_version = props.apiVersion;
    features_.driver_version = props.driverVersion;
    features_.vendor_id = props.vendorID;
    features_.device_id = props.deviceID;
    std::copy(
        std::begin(props.pipelineCacheUUID),
        std::end(props.pipelineCacheUUID),
        features_.pipeline_cache_uuid.begin());
    features_.software_device = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    features_.max_workgroup_invocations = props.limits.maxComputeWorkGroupInvocations;
    features_.max_workgroup_size_x = props.limits.maxComputeWorkGroupSize[0];
    features_.max_workgroup_size_y = props.limits.maxComputeWorkGroupSize[1];
    features_.max_workgroup_size_z = props.limits.maxComputeWorkGroupSize[2];
    features_.max_workgroup_count_x = props.limits.maxComputeWorkGroupCount[0];
    features_.min_storage_buffer_offset_alignment =
        props.limits.minStorageBufferOffsetAlignment;
    features_.max_shared_memory_bytes = props.limits.maxComputeSharedMemorySize;
    features_.max_storage_buffer_range = props.limits.maxStorageBufferRange;

    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props);
    std::uint64_t device_local = 0;
    for (std::uint32_t h = 0; h < mem_props.memoryHeapCount; ++h) {
        if (mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            device_local = sat_max(device_local, mem_props.memoryHeaps[h].size);
        }
    }
    features_.device_local_bytes = device_local;

    // --- Command pool / buffer / fence ---
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    check_vk(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_),
             "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = command_pool_;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    check_vk(vkAllocateCommandBuffers(device_, &cmd_alloc, &command_buffer_),
             "vkAllocateCommandBuffers");

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check_vk(vkCreateFence(device_, &fence_info, nullptr, &fence_), "vkCreateFence");

    // --- Fixed descriptor-set layout (4 storage buffers) + pipeline layout ---
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (std::uint32_t b = 0; b < 4; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    set_layout_info.pBindings = bindings.data();
    check_vk(vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr,
                                         &descriptor_set_layout_),
             "vkCreateDescriptorSetLayout");

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(ChainingPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    check_vk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_),
             "vkCreatePipelineLayout");

    // --- Descriptor pool + one reusable descriptor set ---
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 4;
    VkDescriptorPoolCreateInfo desc_pool_info{};
    desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    desc_pool_info.maxSets = 1;
    desc_pool_info.poolSizeCount = 1;
    desc_pool_info.pPoolSizes = &pool_size;
    check_vk(vkCreateDescriptorPool(device_, &desc_pool_info, nullptr, &descriptor_pool_),
             "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = descriptor_pool_;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &descriptor_set_layout_;
    check_vk(vkAllocateDescriptorSets(device_, &set_alloc, &descriptor_set_),
             "vkAllocateDescriptorSets");

    // DOC0307 §7.2 — pick the largest power-of-two anchor count the per-query
    // bitonic sort can hold in one workgroup: bounded by workgroup invocations
    // and by shared memory (each lane holds a 24-byte AnchorWire + a 4-byte
    // original index). Anchor spans above this capacity are routed to the
    // scalable stable GPU radix path; selected Vulkan lookup never host-sorts.
    {
        gpu_anchor_sort_capacity_ = seed_anchor_bitonic_capacity(
            features_.max_workgroup_invocations,
            features_.max_workgroup_size_x,
            features_.max_shared_memory_bytes);
    }

    features_.available = true;
    std::strncpy(features_.device_name.data(), props.deviceName,
                 features_.device_name.size() - 1u);
    features_.device_name.back() = '\0';
    init_error_.clear();
    bioseq_log<DBG>(
        "[vulkan_runtime] device ready: {} (maxWGInvocations={}, "
        "maxWGSize={}x{}x{}, maxWGCountX={}, maxSharedMem={}B, "
        "maxStorageBufferRange={}B, scalarBlockLayout={}, softwareDevice={})",
        props.deviceName, features_.max_workgroup_invocations,
        features_.max_workgroup_size_x, features_.max_workgroup_size_y,
        features_.max_workgroup_size_z, features_.max_workgroup_count_x,
        features_.max_shared_memory_bytes,
        features_.max_storage_buffer_range,
        features_.scalar_block_layout, features_.software_device);
}

void Runtime::shutdown() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        // DOC0310 — release arena buffers (host-visible, owned via Buffer RAII)
        // and the Phase C descriptor pool before the device goes away.
        phase_c_arena_.reset();
        if (phase_c_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, phase_c_pool_, nullptr);
            phase_c_pool_ = VK_NULL_HANDLE;
        }
        std::vector<PipelineEntry>{}.swap(pipeline_cache_);
        std::vector<PhaseBPipelineOwner>{}.swap(phase_b_pipelines_);
        pipeline_cache_metadata_permit_.release();
        if (descriptor_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (descriptor_set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
        if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
        if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    command_buffer_ = VK_NULL_HANDLE;
    command_pool_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    set_workgroup_size_mvk_ = nullptr;
    metallib_provider_capable_ = false;
#if defined(__APPLE__)
    // Runtime initialization can still fail after vkCreateDevice.  Never
    // leave process-wide Metal IMPs or resident archive authority behind when
    // the owning Vulkan runtime is unavailable or being destroyed.
    rollback_metal_pipeline_archive_interpose_install(
        metal_pipeline_archive_interpose_state());
#endif
}

std::uint32_t Runtime::find_memory_type(
    std::uint32_t type_bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props);
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        const bool type_ok = (type_bits & (1u << i)) != 0;
        const bool flags_ok = (mem_props.memoryTypes[i].propertyFlags & flags) == flags;
        if (type_ok && flags_ok) {
            return i;
        }
    }
    throw VulkanRuntimeError("no compatible Vulkan memory type");
}

// ---------------------------------------------------------------------------
// Buffer allocation
// ---------------------------------------------------------------------------

Buffer Runtime::create_host_buffer(std::uint64_t bytes, VkBufferUsageFlags usage) {
    if (!features_.available) {
        throw VulkanRuntimeError("create_host_buffer on unavailable device");
    }
    if (bytes == 0) bytes = 1;  // zero-sized VkBuffers are invalid

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = bytes;
    buf_info.usage = usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    check_vk(vkCreateBuffer(device_, &buf_info, nullptr, &buffer), "vkCreateBuffer(host)");

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, buffer, &reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = find_memory_type(
        reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_properties);
    const VkMemoryPropertyFlags selected_properties =
        memory_properties.memoryTypes[alloc.memoryTypeIndex].propertyFlags;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        throw VulkanRuntimeError("vkAllocateMemory(host)");
    }
    if (vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyBuffer(device_, buffer, nullptr);
        throw VulkanRuntimeError("vkBindBufferMemory(host)");
    }
    void* mapped = nullptr;
    if (vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyBuffer(device_, buffer, nullptr);
        throw VulkanRuntimeError("vkMapMemory(host)");
    }
    const bool footprint_reflected =
#if defined(__APPLE__)
        (selected_properties & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
#else
        false;
#endif
    return Buffer{this, buffer, memory, bytes, reqs.size,
                  selected_properties, footprint_reflected, mapped};
}

std::uint64_t Runtime::buffer_memory_requirement_locked(
    std::uint64_t bytes, VkBufferUsageFlags usage) const
{
    if (bytes == 0u) bytes = 1u;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    check_vk(vkCreateBuffer(device_, &info, nullptr, &buffer),
             "vkCreateBuffer(requirements)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer, &requirements);
    vkDestroyBuffer(device_, buffer, nullptr);
    return requirements.size;
}

std::uint64_t Runtime::phase_c_arena_allocation_requirement(
    std::uint64_t requested_bytes)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError(
            "phase_c_arena_allocation_requirement on unavailable device");
    }
    const std::uint64_t requested_requirement =
        buffer_memory_requirement_locked(
            requested_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // The Phase-C arena is persistent.  A smaller later case reuses the prior
    // high-water allocation, so its count admission must cover that actual live
    // allocation rather than only the new case's requested VkBuffer size.
    const std::uint64_t current_requirement = phase_c_arena_
        ? phase_c_arena_->device_buffer().allocation_bytes
        : 0u;
    return std::max(requested_requirement, current_requirement);
}

std::uint64_t Runtime::buffer_allocation_requirement(
    std::uint64_t requested_bytes,
    VkBufferUsageFlags usage)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError(
            "buffer_allocation_requirement on unavailable device");
    }
    return buffer_memory_requirement_locked(requested_bytes, usage);
}

Buffer Runtime::create_device_buffer(std::uint64_t bytes, VkBufferUsageFlags usage) {
    if (!features_.available) {
        throw VulkanRuntimeError("create_device_buffer on unavailable device");
    }
    if (bytes == 0) bytes = 1;

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = bytes;
    buf_info.usage = usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    check_vk(vkCreateBuffer(device_, &buf_info, nullptr, &buffer), "vkCreateBuffer(device)");

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, buffer, &reqs);

    // Phase A targets unified memory: prefer device-local that is ALSO
    // host-visible+coherent so the round-trip can write/read it directly.
    // Apple Silicon / MoltenVK always satisfies this; the fallback keeps the
    // buffer host-visible (every Vulkan device exposes at least one
    // host-visible+coherent type) so the direct path still applies.
    std::uint32_t mem_type = 0;
    bool found = false;
    try {
        mem_type = find_memory_type(
            reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        found = true;
    } catch (const VulkanRuntimeError&) {
        found = false;
    }
    if (!found) {
        mem_type = find_memory_type(
            reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_properties);
    const VkMemoryPropertyFlags selected_properties =
        memory_properties.memoryTypes[mem_type].propertyFlags;

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = mem_type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        throw VulkanRuntimeError("vkAllocateMemory(device)");
    }
    if (vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyBuffer(device_, buffer, nullptr);
        throw VulkanRuntimeError("vkBindBufferMemory(device)");
    }
    // Map persistently — on unified memory the device-local buffer is
    // host-visible, so the round-trip writes inputs / reads outputs directly.
    void* mapped = nullptr;
    if (vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        // Not host-visible: leave unmapped. The round-trip rejects this case
        // (Phase A requires unified memory) rather than silently mis-staging.
        mapped = nullptr;
    }
    const bool footprint_reflected =
#if defined(__APPLE__)
        (selected_properties & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
#else
        false;
#endif
    return Buffer{this, buffer, memory, bytes, reqs.size,
                  selected_properties, footprint_reflected, mapped};
}

void Runtime::destroy_buffer(Buffer& buffer) noexcept {
    if (device_ == VK_NULL_HANDLE) return;
    if (buffer.mapped != nullptr && buffer.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, buffer.memory);
    }
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer.memory, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Pipeline creation / cache
// ---------------------------------------------------------------------------

std::uint64_t Runtime::pipeline_cache_metadata_bytes_locked() const noexcept {
    using bioseq::parallelism::saturating_add;
    using bioseq::parallelism::saturating_mul;
    std::uint64_t owned = saturating_mul(
        static_cast<std::uint64_t>(pipeline_cache_.capacity()),
        sizeof(PipelineEntry));
    owned = saturating_add(
        owned,
        saturating_mul(
            static_cast<std::uint64_t>(phase_b_pipelines_.capacity()),
            sizeof(PhaseBPipelineOwner)));
    for (const PipelineEntry& entry : pipeline_cache_) {
        owned = saturating_add(owned, entry.entry_name.capacity());
        if (entry.metal_archive_receipt) {
            owned = saturating_add(
                owned,
                entry.metal_archive_receipt->owned_capacity_bytes());
        }
    }
    for (const PhaseBPipelineOwner& entry : phase_b_pipelines_) {
        owned = saturating_add(owned, entry.entry_name.capacity());
        if (entry.metal_archive_receipt) {
            owned = saturating_add(
                owned,
                entry.metal_archive_receipt->owned_capacity_bytes());
        }
    }
    return owned;
}

void Runtime::admit_pipeline_cache_growth_locked(
    std::size_t normal_capacity,
    std::size_t phase_b_capacity) {
    using bioseq::parallelism::saturating_add;
    using bioseq::parallelism::saturating_mul;
    std::uint64_t projected = pipeline_cache_metadata_bytes_locked();
    if (normal_capacity > pipeline_cache_.capacity()) {
        projected = saturating_add(
            projected,
            saturating_mul(
                static_cast<std::uint64_t>(
                    normal_capacity - pipeline_cache_.capacity()),
                sizeof(PipelineEntry)));
    }
    if (phase_b_capacity > phase_b_pipelines_.capacity()) {
        projected = saturating_add(
            projected,
            saturating_mul(
                static_cast<std::uint64_t>(
                    phase_b_capacity - phase_b_pipelines_.capacity()),
                sizeof(PhaseBPipelineOwner)));
    }
    // Covers one pending entry, receipt allocation/control metadata, all five
    // bounded audit strings, and vector/string allocator headroom.
    projected = saturating_add(
        projected,
        sizeof(RuntimeMetalArchivePipelineReceipt)
            + sizeof(RuntimeMetalArchivePipelineAudit) + 16u * 1024u);
    const std::uint64_t peak = saturating_mul(
        projected, bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    if (peak == (std::numeric_limits<std::uint64_t>::max)()) {
        throw VulkanRuntimeError(
            "pipeline cache metadata admission estimate saturated");
    }
    bioseq::parallelism::MemoryAdmissionFailure failure{};
    if (!pipeline_cache_metadata_permit_) {
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(peak, &failure);
        if (!permit) {
            throw VulkanRuntimeError(shader_cache_message(
                "pipeline cache metadata admission denied before Vulkan creation: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(
                    failure.reason),
                " requested=", failure.requested_bytes));
        }
        pipeline_cache_metadata_permit_ = std::move(*permit);
    } else if (!pipeline_cache_metadata_permit_.grow_to(peak, &failure)) {
        throw VulkanRuntimeError(shader_cache_message(
            "pipeline cache metadata growth denied before Vulkan creation: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(
                failure.reason),
            " requested=", failure.requested_bytes));
    }
    try {
        pipeline_cache_.reserve(normal_capacity);
        phase_b_pipelines_.reserve(phase_b_capacity);
    } catch (...) {
        reconcile_pipeline_cache_metadata_locked();
        throw;
    }
}

void Runtime::reconcile_pipeline_cache_metadata_locked() noexcept {
    if (!pipeline_cache_metadata_permit_) return;
    const std::uint64_t retained = pipeline_cache_metadata_bytes_locked();
    if (retained == (std::numeric_limits<std::uint64_t>::max)()
        || retained > pipeline_cache_metadata_permit_.bytes()) {
        // The conservative preallocation must make this unreachable. Retain
        // the larger permit and fail closed at the next live policy gate.
#if defined(__APPLE__)
        mark_metal_archive_sticky_failure_literal(
            metal_pipeline_archive_interpose_state(),
            "pipeline cache metadata exceeded admitted ownership");
#endif
        return;
    }
    pipeline_cache_metadata_permit_.shrink_to(retained);
    if (retained == 0u) pipeline_cache_metadata_permit_.release();
}

Runtime::ChainingPipeline Runtime::create_pipeline_locked(
    const ChainingPipelineKey& key) {
    const bool provider_used = use_shader_provider_locked();
    const bool archive_receipt_required = provider_used
        || runtime_metal_archive_configuration_installed();
    for (const auto& entry : pipeline_cache_) {
        if (entry.key == key) {
            if (archive_receipt_required) {
                require_immutable_metal_policy_live(
                    "cached chaining pipeline retrieval");
            }
            if (archive_receipt_required
                && (entry.metal_archive_receipt == nullptr
                    || !entry.metal_archive_receipt->accepted())) {
                throw VulkanRuntimeError(
                    "cached chaining pipeline lacks its accepted immutable Metal archive receipt");
            }
            return ChainingPipeline{
                .pipeline = entry.handles.pipeline,
                .shader_key = make_shader_key(
                    ShaderKind::chain_tiled_dp_u64,
                    key.tile_size, key.local_size, 0u),
                .metal_archive_receipt =
                    entry.metal_archive_receipt.get(),
            };
        }
    }

    if (key.local_size == 0 || key.local_size > features_.max_workgroup_invocations) {
        throw VulkanRuntimeError(fmt::format(
            "chaining local_size {} exceeds maxComputeWorkGroupInvocations {}",
            key.local_size, features_.max_workgroup_invocations));
    }

    const std::size_t next_capacity = pipeline_cache_.size()
            < pipeline_cache_.capacity()
        ? pipeline_cache_.capacity()
        : std::max<std::size_t>(
            pipeline_cache_.size() + 1u,
            pipeline_cache_.capacity() == 0u
                ? 1u : pipeline_cache_.capacity() * 2u);
    admit_pipeline_cache_growth_locked(
        next_capacity, phase_b_pipelines_.capacity());
    PipelineCacheAdmissionScope metadata_scope{*this};

    PipelineEntry candidate{};
    candidate.key = key;
    candidate.handles = OwnedPipelineHandles{device_};
    VkShaderModule& module = candidate.handles.module;
    std::string& entry_name = candidate.entry_name;
    std::uint32_t provider_wg_x = 0;
    std::uint32_t provider_wg_y = 0;
    std::uint32_t provider_wg_z = 0;
    const ShaderVariantKey shader_key =
        make_shader_key(ShaderKind::chain_tiled_dp_u64, key.tile_size, key.local_size, 0u);
    if (use_shader_provider_locked()) {
        if (!metallib_provider_capable_) {
            throw VulkanRuntimeError(
                "runtime shader provider installed but vkSetWorkgroupSizeMVK is unavailable");
        }
        RuntimeShaderProvider* provider = runtime_shader_provider_slot();
        auto loaded = provider->load_module(
            device_, shader_key, vkCreateShaderModule);
        if (!loaded) {
            throw VulkanRuntimeError(fmt::format(
                "shader provider failed for {}: {}",
                cache_file_stem(shader_key), loaded.error()));
        }
        module = loaded->module;
        entry_name = loaded->entry_name.empty() ? std::string{"main"} : loaded->entry_name;
        provider_wg_x = loaded->wg_x;
        provider_wg_y = loaded->wg_y;
        provider_wg_z = loaded->wg_z;
        set_workgroup_size_mvk_(module, loaded->wg_x, loaded->wg_y, loaded->wg_z);
    } else {
        std::vector<std::uint32_t> spirv;
        try {
            spirv = compile_shader_spirv(ShaderCompileRequest{
                ShaderKind::chain_tiled_dp_u64, key.tile_size, key.local_size});
        } catch (const std::exception& e) {
            throw VulkanRuntimeError(fmt::format("shader compile failed: {}", e.what()));
        }

        // Validate the direct-anchor ABI in the emitted SPIR-V before trusting the
        // pipeline. A stride/offset mismatch would silently corrupt the DP read of
        // every anchor; reject rather than hide it behind a conversion buffer.
        const SpirvLayoutScan scan = scan_spirv_layout(spirv);
        if (!scan.has_array_stride(24)) {
            throw VulkanRuntimeError("SPIR-V Anchors array stride != 24");
        }
        if (!scan.has_struct_offsets({0u, 8u, 16u, 20u})) {
            throw VulkanRuntimeError("SPIR-V AnchorWire member offsets != {0,8,16,20}");
        }

        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = spirv.size() * sizeof(std::uint32_t);
        module_info.pCode = spirv.data();
        check_vk(vkCreateShaderModule(device_, &module_info, nullptr, &module),
                 "vkCreateShaderModule");
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = entry_name.c_str();

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = pipeline_layout_;

    VkPipeline& pipeline = candidate.handles.pipeline;
    MetalArchivePipelineCreateScope archive_scope{
        archive_receipt_required,
        features_.device_uuid,
        shader_key,
        entry_name};
    if (!archive_scope.ready_for_pipeline_call()) {
        auto rejected = archive_scope.finish(VK_ERROR_INITIALIZATION_FAILED);
        throw VulkanRuntimeError(fmt::format(
            "immutable Metal archive scope rejected chaining pipeline before creation: {}",
            rejected ? "missing required receipt" : rejected.error()));
    }
    const auto create_begin = std::chrono::steady_clock::now();
    const VkResult pr = vkCreateComputePipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
    const auto create_elapsed = std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - create_begin).count();
    candidate.pipeline_create_ns = std::max<std::uint64_t>(
        1u, create_elapsed > 0
            ? static_cast<std::uint64_t>(create_elapsed) : 1u);
    auto archive_verdict = archive_scope.finish(pr);
    if (!archive_verdict) {
        throw VulkanRuntimeError(fmt::format(
            "immutable Metal archive receipt failed for chaining pipeline: {}",
            archive_verdict.error()));
    }
    if (pr != VK_SUCCESS) {
        throw VulkanRuntimeError(
            fmt::format(
                "vkCreateComputePipelines: VkResult {} provider={} entry={} wg={},{},{} vk={} mvk={}",
                static_cast<int>(pr),
                provider_used ? "metallib" : "spirv",
                entry_name,
                provider_wg_x,
                provider_wg_y,
                provider_wg_z,
                symbol_image(reinterpret_cast<const void*>(vkCreateComputePipelines)),
                symbol_image(reinterpret_cast<const void*>(set_workgroup_size_mvk_))));
    }

    if (archive_verdict->has_value()) {
        RuntimeMetalArchivePipelineAudit& audit = **archive_verdict;
        audit.api_version = features_.api_version;
        audit.driver_version = features_.driver_version;
        audit.vendor_id = features_.vendor_id;
        audit.device_id = features_.device_id;
        audit.pipeline_cache_uuid = features_.pipeline_cache_uuid;
        audit.pipeline_create_ns = candidate.pipeline_create_ns;
        candidate.metal_archive_receipt.reset(
            new RuntimeMetalArchivePipelineReceipt(
                std::move(audit)));
    }
    pipeline_cache_.push_back(std::move(candidate));
    try {
        bioseq_log<DBG>(
            "[vulkan_runtime] built chaining pipeline tile={} local={}",
            key.tile_size, key.local_size);
    } catch (...) {
    }
    const PipelineEntry& retained = pipeline_cache_.back();
    return ChainingPipeline{
        .pipeline = retained.handles.pipeline,
        .shader_key = shader_key,
        .metal_archive_receipt = retained.metal_archive_receipt.get(),
    };
}

// ---------------------------------------------------------------------------
// Upload / download helpers (called under mu_, command buffer already open)
// ---------------------------------------------------------------------------

void Runtime::upload_locked(
    Buffer& transfer_scratch, Buffer& dst, std::span<const std::byte> bytes,
    VkAccessFlags dst_access) {
    (void)transfer_scratch;  // unified-memory path has no bounce allocation
    (void)dst_access;  // host-coherent + queue submit makes host writes visible
    if (bytes.empty()) return;
    if (dst.mapped == nullptr) {
        // Phase A requires host-visible device buffers (unified memory). The
        // scratch parameter is retained in the ABI for a future discrete-GPU
        // bounce path; an invalid/default buffer is intentional today.
        throw VulkanRuntimeError("device buffer is not host-visible (Phase A needs unified memory)");
    }
    DEBUG_ASSERT(bytes.size() <= dst.bytes, "upload exceeds destination buffer size");
    if (bytes.data() != static_cast<const std::byte*>(dst.mapped)) {
        std::memcpy(dst.mapped, bytes.data(), bytes.size());
    }
}

void Runtime::download_locked(
    Buffer& transfer_scratch, const Buffer& src, std::span<std::byte> bytes) {
    (void)transfer_scratch;  // unified-memory path has no bounce allocation
    if (bytes.empty()) return;
    if (src.mapped == nullptr) {
        throw VulkanRuntimeError("device buffer is not host-visible (Phase A needs unified memory)");
    }
    DEBUG_ASSERT(bytes.size() <= src.bytes, "download exceeds source buffer size");
    if (bytes.data() != static_cast<std::byte*>(src.mapped)) {
        std::memcpy(bytes.data(), src.mapped, bytes.size());
    }
}

// ---------------------------------------------------------------------------
// The one hot entry point
// ---------------------------------------------------------------------------

void Runtime::run_chaining_tiled_dp_roundtrip(
    const ChainingPipelineKey& key,
    Buffer& transfer_scratch,
    Buffer& anchors,
    Buffer& ranges,
    Buffer& best_scores,
    Buffer& predecessors,
    std::span<const std::byte> anchors_bytes,
    std::span<const std::byte> ranges_bytes,
    std::span<std::byte> best_scores_bytes,
    std::span<std::byte> predecessors_bytes,
    std::uint32_t anchor_count,
    std::uint32_t range_count,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip) {
    std::lock_guard<std::mutex> guard(mu_);

    if (!features_.available) {
        throw VulkanRuntimeError("run_chaining_tiled_dp_roundtrip on unavailable device");
    }
    // DOC0353 (approach E): ONE WORKGROUP PER RANGE; the kernel grid-strides
    // workgroups over ranges, so the dispatch is min(range_count, device limit)
    // workgroups — range_count is just data, no workgroup-count throw.
    const std::uint32_t chain_groups =
        std::min(range_count, features_.max_workgroup_count_x);
    const VkPipeline pipeline = create_pipeline_locked(key).pipeline;

    // H2D: write the anchors/ranges and the caller-initialised best-scores /
    // predecessors straight into the host-visible device buffers.
    upload_locked(transfer_scratch, anchors, anchors_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, ranges, ranges_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, best_scores, best_scores_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    upload_locked(transfer_scratch, predecessors, predecessors_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // Point the reusable descriptor set at this call's four buffers.
    const std::array<VkDescriptorBufferInfo, 4> buffer_infos{{
        {anchors.buffer, 0, VK_WHOLE_SIZE},
        {ranges.buffer, 0, VK_WHOLE_SIZE},
        {best_scores.buffer, 0, VK_WHOLE_SIZE},
        {predecessors.buffer, 0, VK_WHOLE_SIZE},
    }};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (std::uint32_t b = 0; b < 4; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = descriptor_set_;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &buffer_infos[b];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    ChainingPushConstants push{};
    push.anchor_count = anchor_count;
    push.range_count = range_count;
    push.max_dist_x = max_dist_x;
    push.max_dist_y = max_dist_y;
    push.bandwidth = bandwidth;
    push.max_iter = max_iter;
    push.gap_penalty = gap_penalty;
    push.skip_penalty = skip_penalty;
    push.max_chain_skip = max_chain_skip;

    // Record: host-write barrier -> dispatch -> shader-write->host-read barrier.
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host_to_shader,
                         0, nullptr, 0, nullptr);

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
    vkCmdPushConstants(command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(command_buffer_, chain_groups, 1, 1);

    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &shader_to_host,
                         0, nullptr, 0, nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    require_immutable_metal_policy_live("chaining operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences");

    // D2H: read the propagated scores / predecessors back.
    download_locked(transfer_scratch, best_scores, best_scores_bytes);
    download_locked(transfer_scratch, predecessors, predecessors_bytes);

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
}

GpuCoordinateChainTraceResult Runtime::run_chaining_coordinate_trace_roundtrip(
    const ChainingPipelineKey& key,
    Buffer& transfer_scratch,
    Buffer& anchors,
    Buffer& ranges,
    Buffer& best_scores,
    Buffer& predecessors,
    std::span<const std::byte> anchors_bytes,
    std::span<const std::byte> ranges_bytes,
    std::span<const std::byte> best_scores_bytes,
    std::span<const std::byte> predecessors_bytes,
    std::uint32_t anchor_count,
    std::uint32_t range_count,
    std::uint64_t query_len,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit)
{
    std::lock_guard<std::mutex> guard(mu_);

    GpuCoordinateChainTraceResult out{};
    if (!features_.available) {
        throw VulkanRuntimeError("run_chaining_coordinate_trace_roundtrip on unavailable device");
    }

    const std::uint32_t chain_groups =
        std::min(range_count, features_.max_workgroup_count_x);
    const VkPipeline pipeline = create_pipeline_locked(key).pipeline;
    const PhaseBPipeline coord_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_coordinate_reduce, 128u, 1u);

    upload_locked(transfer_scratch, anchors, anchors_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, ranges, ranges_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, best_scores, best_scores_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    upload_locked(transfer_scratch, predecessors, predecessors_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    constexpr VkBufferUsageFlags coordinate_trace_buffer_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Buffer visited = create_device_buffer(
        std::uint64_t{anchor_count} * sizeof(std::uint32_t),
        coordinate_trace_buffer_usage);
    Buffer trace = create_device_buffer(
        std::uint64_t{anchor_count} * sizeof(std::uint32_t),
        coordinate_trace_buffer_usage);
    Buffer result_buf = create_device_buffer(
        sizeof(GpuCoordinateOnlyResultWire), coordinate_trace_buffer_usage);
    if (visited.mapped == nullptr || trace.mapped == nullptr || result_buf.mapped == nullptr) {
        throw VulkanRuntimeError("coordinate-only chaining requires host-visible trace buffers");
    }
    std::memset(visited.mapped, 0, static_cast<std::size_t>(visited.bytes));
    std::memset(result_buf.mapped, 0, static_cast<std::size_t>(result_buf.bytes));

    const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
        {anchors.buffer, 0, VK_WHOLE_SIZE},
        {ranges.buffer, 0, VK_WHOLE_SIZE},
        {best_scores.buffer, 0, VK_WHOLE_SIZE},
        {predecessors.buffer, 0, VK_WHOLE_SIZE},
    }};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (std::uint32_t b = 0; b < 4; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = descriptor_set_;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &chain_infos[b];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    ChainingPushConstants chain_push{};
    chain_push.anchor_count = anchor_count;
    chain_push.range_count = range_count;
    chain_push.max_dist_x = max_dist_x;
    chain_push.max_dist_y = max_dist_y;
    chain_push.bandwidth = bandwidth;
    chain_push.max_iter = max_iter;
    chain_push.gap_penalty = gap_penalty;
    chain_push.skip_penalty = skip_penalty;
    chain_push.max_chain_skip = max_chain_skip;

    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(chain_coord)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(chain_coord)");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        command_buffer_,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1,
        &host_to_shader,
        0,
        nullptr,
        0,
        nullptr);

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
    vkCmdPushConstants(command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(chain_push), &chain_push);
    vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
    VkMemoryBarrier shader_to_shader{};
    shader_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_shader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_shader.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        command_buffer_,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1,
        &shader_to_shader,
        0,
        nullptr,
        0,
        nullptr);

    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
        {anchors.buffer, 0, VK_WHOLE_SIZE},
        {best_scores.buffer, 0, VK_WHOLE_SIZE},
        {predecessors.buffer, 0, VK_WHOLE_SIZE},
        {visited.buffer, 0, visited.bytes},
        {trace.buffer, 0, trace.bytes},
        {result_buf.buffer, 0, result_buf.bytes},
    }};
    GpuCoordinateReducePush coord_push{
        .anchor_count = anchor_count,
        .query_len_lo = static_cast<std::uint32_t>(query_len & 0xffffffffull),
        .query_len_hi = static_cast<std::uint32_t>(query_len >> 32),
        .min_chain_score = min_chain_score,
        .min_seed_count = min_seed_count,
        .probe_limit = probe_limit,
        .reserved = 0u,
    };
    record_phase_c_dispatch_locked(
        command_buffer_, coord_pipe, coord_infos,
        &coord_push, sizeof(coord_push), 1u);
    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(
        command_buffer_,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        1,
        &shader_to_host,
        0,
        nullptr,
        0,
        nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "end(chain_coord)");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "submit(chain_coord)");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX),
             "wait(chain_coord)");
    require_immutable_metal_policy_live(
        "chaining coordinate operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "reset_fence(chain_coord)");

    std::memcpy(&out.result, result_buf.mapped, sizeof(out.result));
    if (out.result.valid != 0u) {
        if (out.result.seed_count < 0
            || static_cast<std::uint64_t>(out.result.seed_count) > anchor_count) {
            throw VulkanRuntimeError("coordinate-only chaining produced invalid trace length");
        }
        const auto trace_count = static_cast<std::size_t>(out.result.seed_count);
        out.trace.resize(trace_count);
        if (trace_count > 0) {
            std::memcpy(out.trace.data(), trace.mapped, trace_count * sizeof(std::uint32_t));
        }
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(chain_coord_post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// ---------------------------------------------------------------------------
// DOC0307 Phase B — seeding roundtrips
// ---------------------------------------------------------------------------

namespace {

struct SeedLookupPush {
    std::uint32_t minimizer_count;
    std::uint32_t words32_per_minimizer;
    std::uint32_t word_kind;
    std::uint32_t hash_shift;
    std::uint32_t pos_bits;
    std::uint32_t query_len;
    std::uint32_t kmer_size;
    std::uint32_t slot_mask;
    std::uint32_t bucket_count;
    std::uint32_t exact_anchor_count;
    std::uint32_t target_ref_bits;
};
static_assert(sizeof(SeedLookupPush) == 44);

constexpr VkBufferUsageFlags kPhaseBBufferUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT;

struct AlignmentExtensionPush {
    std::uint32_t job_count = 0;
    std::uint32_t max_query_len = 0;
    std::uint32_t max_target_len = 0;
    std::uint32_t max_band_width = 0;
    std::uint32_t alphabet_size = 0;
    std::int32_t match_score = 0;
    std::int32_t mismatch_penalty = 0;
    std::int32_t gap_open_penalty = 0;
    std::int32_t gap_extend_penalty = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved0 = 0;
};
static_assert(sizeof(AlignmentExtensionPush) == 44);
static_assert(std::is_standard_layout_v<AlignmentExtensionPush>);

struct AlignmentCigarPush {
    std::uint32_t job_count = 0;
    std::uint32_t alphabet_size = 0;
    std::uint32_t max_band_width = 0;
    std::uint32_t tile_query = 0;
    std::uint32_t tile_target = 0;
    std::int32_t match_score = 0;
    std::int32_t mismatch_penalty = 0;
    std::int32_t gap_open_short = 0;
    std::int32_t gap_extend_short = 0;
    std::int32_t gap_open_long = 0;
    std::int32_t gap_extend_long = 0;
    std::int32_t z_drop = 0;
    std::int32_t end_bonus = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};
static_assert(sizeof(AlignmentCigarPush) == 60);
static_assert(std::is_standard_layout_v<AlignmentCigarPush>);

[[nodiscard]] AlignmentExtensionPush alignment_extension_push(
    std::uint32_t job_count,
    const GpuAlignmentExtensionParams& params) noexcept
{
    return AlignmentExtensionPush{
        job_count,
        params.max_query_len,
        params.max_target_len,
        params.max_band_width,
        params.alphabet_size,
        params.match_score,
        params.mismatch_penalty,
        params.gap_open_penalty,
        params.gap_extend_penalty,
        params.flags,
        params.reserved0};
}

[[nodiscard]] AlignmentCigarPush alignment_cigar_push(
    std::uint32_t job_count,
    const GpuAlignmentCigarParams& params) noexcept
{
    return AlignmentCigarPush{
        job_count,
        params.alphabet_size,
        params.max_band_width,
        params.tile_query,
        params.tile_target,
        params.match_score,
        params.mismatch_penalty,
        params.gap_open_short,
        params.gap_extend_short,
        params.gap_open_long,
        params.gap_extend_long,
        params.z_drop,
        params.end_bonus,
        params.flags,
        params.reserved};
}

void validate_alignment_extension_score(const char* name, std::int32_t value) {
    constexpr std::int32_t kMaxAbsScore = 1'000'000;
    if (value < 0 || value > kMaxAbsScore) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension {} {} out of range (0..{})",
            name, value, kMaxAbsScore));
    }
}

void validate_alignment_extension_symbols(
    std::span<const std::uint32_t> symbols,
    std::uint32_t offset,
    std::uint32_t length,
    std::uint32_t alphabet_size,
    const char* label,
    std::size_t job_index)
{
    const std::uint64_t begin = offset;
    const std::uint64_t end = begin + static_cast<std::uint64_t>(length);
    if (begin > symbols.size() || end > symbols.size()) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension job {} {} range [{}, {}) exceeds symbol count {}",
            job_index, label, begin, end, symbols.size()));
    }
    for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint32_t symbol = symbols[static_cast<std::size_t>(offset) + i];
        if (symbol >= alphabet_size) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension job {} {} symbol {} at local offset {} exceeds alphabet_size {}",
                job_index, label, symbol, i, alphabet_size));
        }
    }
}

void validate_alignment_extension_inputs(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const GpuAlignmentExtensionJobWire> jobs,
    const GpuAlignmentExtensionParams& params)
{
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("alignment extension job_count exceeds uint32");
    }
    if (query_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("alignment extension query symbol stream exceeds uint32 offsets");
    }
    if (target_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("alignment extension target symbol stream exceeds uint32 offsets");
    }
    if (params.max_query_len == 0
        || params.max_query_len > k_gpu_alignment_extension_max_len) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension max_query_len {} out of range (1..{})",
            params.max_query_len, k_gpu_alignment_extension_max_len));
    }
    if (params.max_target_len == 0
        || params.max_target_len > k_gpu_alignment_extension_max_len) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension max_target_len {} out of range (1..{})",
            params.max_target_len, k_gpu_alignment_extension_max_len));
    }
    if (params.max_band_width > k_gpu_alignment_extension_max_band_width) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension max_band_width {} exceeds {}",
            params.max_band_width, k_gpu_alignment_extension_max_band_width));
    }
    if (params.alphabet_size == 0 || params.alphabet_size > 256u) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension alphabet_size {} out of range (1..256)",
            params.alphabet_size));
    }
    validate_alignment_extension_score("match_score", params.match_score);
    validate_alignment_extension_score("mismatch_penalty", params.mismatch_penalty);
    validate_alignment_extension_score("gap_open_penalty", params.gap_open_penalty);
    validate_alignment_extension_score("gap_extend_penalty", params.gap_extend_penalty);
    if (params.match_score == 0) {
        throw VulkanRuntimeError("alignment extension match_score must be positive");
    }
    if (params.flags != 0 || params.reserved0 != 0) {
        throw VulkanRuntimeError("alignment extension params flags/reserved must be zero");
    }

    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const GpuAlignmentExtensionJobWire& job = jobs[i];
        if (job.query_len > params.max_query_len) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension job {} query_len {} exceeds max_query_len {}",
                i, job.query_len, params.max_query_len));
        }
        if (job.target_len > params.max_target_len) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension job {} target_len {} exceeds max_target_len {}",
                i, job.target_len, params.max_target_len));
        }
        if (job.band_width > params.max_band_width) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension job {} band_width {} exceeds max_band_width {}",
                i, job.band_width, params.max_band_width));
        }
        if (job.flags != 0 || job.reserved0 != 0 || job.reserved1 != 0) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension job {} flags/reserved must be zero", i));
        }
        validate_alignment_extension_symbols(
            query_symbols, job.query_offset, job.query_len,
            params.alphabet_size, "query", i);
        validate_alignment_extension_symbols(
            target_symbols, job.target_offset, job.target_len,
            params.alphabet_size, "target", i);
    }
}

void validate_sneaky_snake_inputs(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const GpuSneakySnakeJobWire> jobs)
{
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("SneakySnake job_count exceeds uint32");
    }
    if (query_ranks.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError(
            "SneakySnake query rank stream exceeds uint32 offsets");
    }
    if (target_ranks.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError(
            "SneakySnake target rank stream exceeds uint32 offsets");
    }

    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const GpuSneakySnakeJobWire& job = jobs[i];
        const std::uint64_t query_end =
            static_cast<std::uint64_t>(job.query_offset)
            + static_cast<std::uint64_t>(job.query_length);
        const std::uint64_t target_end =
            static_cast<std::uint64_t>(job.target_offset)
            + static_cast<std::uint64_t>(job.target_length);
        if (job.query_offset > query_ranks.size()
            || query_end > query_ranks.size()) {
            throw VulkanRuntimeError(fmt::format(
                "SneakySnake job {} query range [{}, {}) exceeds rank count {}",
                i, job.query_offset, query_end, query_ranks.size()));
        }
        if (job.target_offset > target_ranks.size()
            || target_end > target_ranks.size()) {
            throw VulkanRuntimeError(fmt::format(
                "SneakySnake job {} target range [{}, {}) exceeds rank count {}",
                i, job.target_offset, target_end, target_ranks.size()));
        }
    }
}

void validate_sneaky_snake_results(
    std::span<const GpuSneakySnakeJobWire> jobs,
    std::span<const GpuSneakySnakeResultWire> results)
{
    if (results.size() != jobs.size()) {
        throw VulkanRuntimeError("SneakySnake result count mismatch");
    }
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const GpuSneakySnakeJobWire& job = jobs[i];
        const GpuSneakySnakeResultWire& result = results[i];
        if (result.accepted > 1u || result.reserved != 0u) {
            throw VulkanRuntimeError(fmt::format(
                "SneakySnake shader returned malformed flags for job {}",
                i));
        }

        if (result.status == k_gpu_sneaky_snake_status_unsupported) {
            if (job.max_edits <= k_gpu_sneaky_snake_max_edits
                || result.lower_bound != 0u || result.accepted != 1u) {
                throw VulkanRuntimeError(fmt::format(
                    "SneakySnake shader returned invalid unsupported result for job {}",
                    i));
            }
            continue;
        }

        if (job.max_edits > k_gpu_sneaky_snake_max_edits) {
            throw VulkanRuntimeError(fmt::format(
                "SneakySnake shader failed to retain unsupported job {}",
                i));
        }
        if (result.status == k_gpu_sneaky_snake_status_length_difference_only) {
            if (job.query_length == job.target_length) {
                throw VulkanRuntimeError(fmt::format(
                    "SneakySnake shader returned unequal-length status for equal job {}",
                    i));
            }
            const std::uint32_t length_difference =
                job.query_length > job.target_length
                ? job.query_length - job.target_length
                : job.target_length - job.query_length;
            const std::uint32_t expected_accepted =
                length_difference <= job.max_edits ? 1u : 0u;
            if (result.lower_bound != length_difference
                || result.accepted != expected_accepted) {
                throw VulkanRuntimeError(fmt::format(
                    "SneakySnake shader returned invalid length-difference result for job {}",
                    i));
            }
            continue;
        }
        if (result.status != k_gpu_sneaky_snake_status_ok
            || job.query_length != job.target_length) {
            throw VulkanRuntimeError(fmt::format(
                "SneakySnake shader returned invalid status {} for job {}",
                result.status, i));
        }
        const std::uint32_t expected_accepted =
            result.lower_bound <= job.max_edits ? 1u : 0u;
        if (result.accepted != expected_accepted
            || result.lower_bound > job.max_edits + 1u) {
            throw VulkanRuntimeError(fmt::format(
                "SneakySnake shader returned invalid equal-length result for job {}",
                i));
        }
    }
}

void validate_alignment_cigar_score(const char* name, std::int32_t value) {
    constexpr std::int32_t kMaxAbsScore = 1'000'000;
    if (value < 0 || value > kMaxAbsScore) {
        throw VulkanRuntimeError(fmt::format(
            "alignment CIGAR extension {} {} out of range (0..{})",
            name, value, kMaxAbsScore));
    }
}

void validate_alignment_cigar_inputs(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const GpuAlignmentCigarJobWire> jobs,
    const GpuAlignmentCigarParams& params,
    std::uint32_t packed_cigar_capacity)
{
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("alignment CIGAR extension job_count exceeds uint32");
    }
    if (query_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError(
            "alignment CIGAR extension query symbol stream exceeds uint32 offsets");
    }
    if (target_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError(
            "alignment CIGAR extension target symbol stream exceeds uint32 offsets");
    }
    if (params.alphabet_size == 0 || params.alphabet_size > 256u) {
        throw VulkanRuntimeError(fmt::format(
            "alignment CIGAR extension alphabet_size {} out of range (1..256)",
            params.alphabet_size));
    }
    if (params.max_band_width > k_gpu_alignment_extension_max_band_width) {
        throw VulkanRuntimeError(fmt::format(
            "alignment CIGAR extension max_band_width {} exceeds {}",
            params.max_band_width, k_gpu_alignment_extension_max_band_width));
    }
    if (params.tile_query == 0 || params.tile_query > k_gpu_alignment_extension_max_len
        || params.tile_target == 0
        || params.tile_target > k_gpu_alignment_extension_max_len) {
        throw VulkanRuntimeError(fmt::format(
            "alignment CIGAR extension tile geometry {}x{} out of range (1..{})",
            params.tile_query, params.tile_target, k_gpu_alignment_extension_max_len));
    }
    validate_alignment_cigar_score("match_score", params.match_score);
    validate_alignment_cigar_score("mismatch_penalty", params.mismatch_penalty);
    validate_alignment_cigar_score("gap_open_short", params.gap_open_short);
    validate_alignment_cigar_score("gap_extend_short", params.gap_extend_short);
    validate_alignment_cigar_score("gap_open_long", params.gap_open_long);
    validate_alignment_cigar_score("gap_extend_long", params.gap_extend_long);
    validate_alignment_cigar_score("z_drop", params.z_drop);
    validate_alignment_cigar_score("end_bonus", params.end_bonus);
    if (params.match_score == 0) {
        throw VulkanRuntimeError("alignment CIGAR extension match_score must be positive");
    }
    if (params.flags != 0 || params.reserved != 0) {
        throw VulkanRuntimeError("alignment CIGAR extension params flags/reserved must be zero");
    }
    if (packed_cigar_capacity == 0) {
        throw VulkanRuntimeError("alignment CIGAR extension packed CIGAR capacity is zero");
    }

    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const GpuAlignmentCigarJobWire& job = jobs[i];
        if (job.query_len > k_gpu_alignment_simple_cigar_max_len
            || job.target_len > k_gpu_alignment_simple_cigar_max_len) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} length {}x{} exceeds {}",
                i, job.query_len, job.target_len,
                k_gpu_alignment_simple_cigar_max_len));
        }
        if (job.band_width > params.max_band_width) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} band_width {} exceeds max_band_width {}",
                i, job.band_width, params.max_band_width));
        }
        if (job.cigar_capacity == 0) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} CIGAR capacity is zero", i));
        }
        const bool bounded_dp_candidate =
            job.query_len <= k_gpu_alignment_extension_max_len
            && job.target_len <= k_gpu_alignment_extension_max_len;
        const std::uint64_t traceback_cells = bounded_dp_candidate
            ? (static_cast<std::uint64_t>(job.query_len) + 1u)
                * (static_cast<std::uint64_t>(job.target_len) + 1u)
            : 0u;
        const std::uint64_t traceback_end =
            static_cast<std::uint64_t>(job.traceback_tile_offset)
            + static_cast<std::uint64_t>(job.traceback_tile_count);
        if (bounded_dp_candidate && job.traceback_tile_count < traceback_cells) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} traceback capacity {} below required {}",
                i, job.traceback_tile_count, traceback_cells));
        }
        if (traceback_end > std::numeric_limits<std::uint32_t>::max()) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} traceback range [{}, {}) exceeds uint32",
                i, job.traceback_tile_offset, traceback_end));
        }
        const std::uint64_t cigar_end =
            static_cast<std::uint64_t>(job.cigar_offset)
            + static_cast<std::uint64_t>(job.cigar_capacity);
        if (cigar_end > packed_cigar_capacity) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} CIGAR range [{}, {}) exceeds capacity {}",
                i, job.cigar_offset, cigar_end, packed_cigar_capacity));
        }
        validate_alignment_extension_symbols(
            query_symbols, job.query_symbol_offset, job.query_len,
            params.alphabet_size, "query", i);
        validate_alignment_extension_symbols(
            target_symbols, job.target_symbol_offset, job.target_len,
            params.alphabet_size, "target", i);
    }
}

/// Device byte footprint for `count` packed minimizers of the given word kind.
[[nodiscard]] std::uint64_t minimizer_device_bytes(
    std::uint32_t word_kind, std::uint32_t words32_per_minimizer,
    std::uint64_t count) noexcept
{
    if (word_kind == 0u) {  // u16: two minimizers packed per 32-bit word
        return ((count + 1u) / 2u) * sizeof(std::uint32_t);
    }
    return count * static_cast<std::uint64_t>(words32_per_minimizer)
         * sizeof(std::uint32_t);
}

/// Host byte footprint for `count` minimizers (one packed element each).
[[nodiscard]] std::uint64_t minimizer_host_bytes(
    std::uint32_t word_kind, std::uint64_t count) noexcept
{
    switch (word_kind) {
        case 0u:  return count * 2u;   // u16
        case 1u:  return count * 4u;   // u32
        case 2u:  return count * 8u;   // u64
        default:  return count * 16u;  // u64x2
    }
}

}  // namespace

Runtime::PhaseBPipeline Runtime::get_phase_b_pipeline_locked(
    ShaderKind kind, std::uint32_t local_size, std::uint32_t max_span)
{
    const bool provider_used = use_shader_provider_locked();
    const bool archive_receipt_required = provider_used
        || runtime_metal_archive_configuration_installed();
    for (auto& entry : phase_b_pipelines_) {
        if (entry.kind == kind && entry.local_size == local_size
            && entry.max_span == max_span) {
            if (archive_receipt_required) {
                require_immutable_metal_policy_live(
                    "cached compute pipeline retrieval");
            }
            if (archive_receipt_required
                && (entry.metal_archive_receipt == nullptr
                    || !entry.metal_archive_receipt->accepted())) {
                throw VulkanRuntimeError(
                    "cached compute pipeline lacks its accepted immutable Metal archive receipt");
            }
            return entry.view();
        }
    }
    const ShaderVariantKey shader_key = make_shader_key(kind, 384u, local_size, max_span);
    ShaderCompileRequest geometry_request{};
    geometry_request.kind = kind;
    geometry_request.tile_size = 384u;
    geometry_request.local_size = local_size;
    geometry_request.max_span = max_span;
    const ShaderWorkgroupSize workgroup =
        expected_shader_workgroup_size(geometry_request);
    const std::uint64_t workgroup_invocations =
        static_cast<std::uint64_t>(workgroup.x)
        * static_cast<std::uint64_t>(workgroup.y)
        * static_cast<std::uint64_t>(workgroup.z);
    if (workgroup.x == 0 || workgroup.y == 0 || workgroup.z == 0
        || workgroup.x > features_.max_workgroup_size_x
        || workgroup.y > features_.max_workgroup_size_y
        || workgroup.z > features_.max_workgroup_size_z
        || workgroup_invocations > features_.max_workgroup_invocations) {
        throw VulkanRuntimeError(fmt::format(
            "Phase B workgroup {}x{}x{} (key local_size {}) exceeds device "
            "limits maxComputeWorkGroupSize={}x{}x{} or "
            "maxComputeWorkGroupInvocations={}",
            workgroup.x, workgroup.y, workgroup.z, local_size,
            features_.max_workgroup_size_x, features_.max_workgroup_size_y,
            features_.max_workgroup_size_z,
            features_.max_workgroup_invocations));
    }

    const DescriptorLayoutKind layout_kind = descriptor_layout_for(kind);
    const std::uint32_t binding_count = descriptor_binding_count(layout_kind);
    const std::uint32_t push_size = push_constant_size(kind);

    const std::size_t next_capacity = phase_b_pipelines_.size()
            < phase_b_pipelines_.capacity()
        ? phase_b_pipelines_.capacity()
        : std::max<std::size_t>(
            phase_b_pipelines_.size() + 1u,
            phase_b_pipelines_.capacity() == 0u
                ? 1u : phase_b_pipelines_.capacity() * 2u);
    admit_pipeline_cache_growth_locked(
        pipeline_cache_.capacity(), next_capacity);
    PipelineCacheAdmissionScope metadata_scope{*this};

    PhaseBPipelineOwner entry;
    entry.handles = OwnedPipelineHandles{device_};
    entry.kind = kind;
    entry.local_size = local_size;
    entry.max_span = max_span;
    entry.binding_count = binding_count;
    entry.push_size = push_size;

    std::vector<VkDescriptorSetLayoutBinding> bindings(binding_count);
    for (std::uint32_t b = 0; b < binding_count; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = binding_count;
    set_layout_info.pBindings = bindings.data();
    check_vk(vkCreateDescriptorSetLayout(
                 device_, &set_layout_info, nullptr,
                 &entry.handles.set_layout),
             "vkCreateDescriptorSetLayout(phase_b)");

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = push_size;
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &entry.handles.set_layout;
    layout_info.pushConstantRangeCount = push_size > 0 ? 1u : 0u;
    layout_info.pPushConstantRanges = push_size > 0 ? &push_range : nullptr;
    check_vk(vkCreatePipelineLayout(
                 device_, &layout_info, nullptr,
                 &entry.handles.pipeline_layout),
             "vkCreatePipelineLayout(phase_b)");

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = binding_count;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    check_vk(vkCreateDescriptorPool(
                 device_, &pool_info, nullptr,
                 &entry.handles.descriptor_pool),
             "vkCreateDescriptorPool(phase_b)");

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = entry.handles.descriptor_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &entry.handles.set_layout;
    check_vk(vkAllocateDescriptorSets(device_, &set_alloc, &entry.descriptor_set),
             "vkAllocateDescriptorSets(phase_b)");

    if (provider_used) {
        if (!metallib_provider_capable_) {
            throw VulkanRuntimeError(
                "runtime shader provider installed but vkSetWorkgroupSizeMVK is unavailable");
        }
        RuntimeShaderProvider* provider = runtime_shader_provider_slot();
        auto loaded = provider->load_module(
            device_, shader_key, vkCreateShaderModule);
        if (!loaded) {
            throw VulkanRuntimeError(fmt::format(
                "shader provider failed for {}: {}",
                cache_file_stem(shader_key), loaded.error()));
        }
        entry.handles.module = loaded->module;
        entry.entry_name =
            loaded->entry_name.empty() ? std::string{"main"} : loaded->entry_name;
        set_workgroup_size_mvk_(
            entry.handles.module,
            loaded->wg_x, loaded->wg_y, loaded->wg_z);
    } else {
        std::vector<std::uint32_t> spirv;
        try {
            ShaderCompileRequest request{};
            request.kind = kind;
            request.tile_size = 384;
            request.local_size = local_size;
            request.max_span = max_span;
            spirv = compile_shader_spirv(request);
        } catch (const std::exception& compile_error) {
            throw VulkanRuntimeError(fmt::format("phase B shader compile failed: {}",
                                                 compile_error.what()));
        }

        // Pin the SSBO wire ABI: a sort-range-stride mismatch would silently
        // corrupt direct SortRangeWire reads.
        const SpirvLayoutScan scan = scan_spirv_layout(spirv);
        if (kind == ShaderKind::seed_lookup_anchors
            && (!scan.has_binding(0) || !scan.has_binding(1)
                || !scan.has_binding(2) || !scan.has_binding(3)
                || !scan.has_binding(4) || !scan.has_binding(5)
                || !scan.has_array_stride(24) || !scan.has_array_stride(8)
                || !scan.has_struct_offsets({0u, 8u, 16u, 20u}))) {
            throw VulkanRuntimeError("SPIR-V seed lookup anchor/key ABI mismatch");
        }
        if (kind == ShaderKind::sort_anchors_bitonic && !scan.has_array_stride(8)) {
            throw VulkanRuntimeError("SPIR-V SortRange array stride != 8");
        }
        if (kind == ShaderKind::generate_units &&
             (!scan.has_binding(0) || !scan.has_binding(1) ||
             !scan.has_array_stride(max_span / 8u))) {
            throw VulkanRuntimeError("SPIR-V generate_units layout mismatch");
        }
        if (kind == ShaderKind::align_extend_banded_affine &&
            (!scan.has_binding(0) || !scan.has_binding(1) ||
             !scan.has_binding(2) || !scan.has_binding(3) ||
             !scan.has_array_stride(4) || !scan.has_array_stride(16) ||
             !scan.has_array_stride(32))) {
            throw VulkanRuntimeError("SPIR-V alignment extension layout mismatch");
        }
        if (kind == ShaderKind::align_extend_simple_cigar &&
            (!scan.has_binding(0) || !scan.has_binding(1) ||
             !scan.has_binding(2) || !scan.has_binding(3) ||
             !scan.has_binding(4) || !scan.has_binding(5) ||
             !scan.has_array_stride(4) ||
             !scan.has_array_stride(40) || !scan.has_array_stride(48))) {
            throw VulkanRuntimeError("SPIR-V alignment CIGAR extension layout mismatch");
        }
        if (kind == ShaderKind::sneaky_snake_rank_batch &&
            (!scan.has_binding_array_stride(0u, 0u, 4u) ||
             !scan.has_binding_array_stride(0u, 1u, 4u) ||
             !scan.has_binding_array_stride(0u, 2u, 20u) ||
             !scan.has_binding_struct_offsets(
                 0u, 2u,
                 std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}) ||
             !scan.has_binding_array_stride(0u, 3u, 16u) ||
             !scan.has_binding_struct_offsets(
                 0u, 3u,
                 std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}) ||
             !scan.has_push_constant_offsets(
                 std::array<std::uint32_t, 1>{0u}))) {
            throw VulkanRuntimeError(
                "SPIR-V SneakySnake rank-batch layout mismatch");
        }

        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = spirv.size() * sizeof(std::uint32_t);
        module_info.pCode = spirv.data();
        check_vk(vkCreateShaderModule(
                     device_, &module_info, nullptr,
                     &entry.handles.module),
                 "vkCreateShaderModule(phase_b)");
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = entry.handles.module;
    stage.pName = entry.entry_name.c_str();
    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = entry.handles.pipeline_layout;
    MetalArchivePipelineCreateScope archive_scope{
        archive_receipt_required,
        features_.device_uuid,
        shader_key,
        entry.entry_name};
    if (!archive_scope.ready_for_pipeline_call()) {
        auto rejected = archive_scope.finish(VK_ERROR_INITIALIZATION_FAILED);
        throw VulkanRuntimeError(fmt::format(
            "immutable Metal archive scope rejected compute pipeline before creation: {}",
            rejected ? "missing required receipt" : rejected.error()));
    }
    const auto create_begin = std::chrono::steady_clock::now();
    const VkResult pr = vkCreateComputePipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        &entry.handles.pipeline);
    const auto create_elapsed = std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - create_begin).count();
    entry.pipeline_create_ns = std::max<std::uint64_t>(
        1u, create_elapsed > 0
            ? static_cast<std::uint64_t>(create_elapsed) : 1u);
    auto archive_verdict = archive_scope.finish(pr);
    if (!archive_verdict) {
        throw VulkanRuntimeError(fmt::format(
            "immutable Metal archive receipt failed for compute pipeline: {}",
            archive_verdict.error()));
    }
    if (pr != VK_SUCCESS) {
        throw VulkanRuntimeError(fmt::format(
            "vkCreateComputePipelines(compute): VkResult {}", static_cast<int>(pr)));
    }
    if (archive_verdict->has_value()) {
        RuntimeMetalArchivePipelineAudit& audit = **archive_verdict;
        audit.api_version = features_.api_version;
        audit.driver_version = features_.driver_version;
        audit.vendor_id = features_.vendor_id;
        audit.device_id = features_.device_id;
        audit.pipeline_cache_uuid = features_.pipeline_cache_uuid;
        audit.pipeline_create_ns = entry.pipeline_create_ns;
        entry.metal_archive_receipt.reset(
            new RuntimeMetalArchivePipelineReceipt(
                std::move(audit)));
    }

    phase_b_pipelines_.push_back(std::move(entry));
    try {
        bioseq_log<DBG>(
            "[vulkan_runtime] built compute pipeline kind={} local={} max_span={}",
            static_cast<int>(kind), local_size, max_span);
    } catch (...) {
    }
    return phase_b_pipelines_.back().view();
}

void Runtime::bind_phase_b_descriptors_locked(
    const PhaseBPipeline& entry, std::span<const VkBuffer> buffers)
{
    DEBUG_ASSERT(buffers.size() == entry.binding_count,
                 "phase B descriptor binding count mismatch");
    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (std::size_t b = 0; b < buffers.size(); ++b) {
        infos[b] = VkDescriptorBufferInfo{buffers[b], 0, VK_WHOLE_SIZE};
        writes[b] = VkWriteDescriptorSet{};
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = entry.descriptor_set;
        writes[b].dstBinding = static_cast<std::uint32_t>(b);
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

SeedLookupResult Runtime::run_seed_lookup_roundtrip(
    RuntimeSeedLookupInputs inputs,
    std::span<const std::uint32_t> prefix_counts,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::uint64_t exact_anchor_count,
    const SeedAnchorSortPlan& sort_plan)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_seed_lookup_roundtrip on unavailable device");
    }
    if (prefix_counts.size() < 2 || exact_anchor_count == 0) {
        return SeedLookupResult{};
    }
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw VulkanRuntimeError("seed lookup missing resident index buffers");
    }
    if (static_cast<std::uint64_t>(inputs.position_count) * 8u
        > inputs.positions->bytes) {
        throw VulkanRuntimeError(
            "seed lookup resident position range is malformed");
    }
    const std::uint64_t slot_count_from_bytes = inputs.slots->bytes / 16u;
    if (inputs.bucket_count != 0u) {
        if (inputs.slot_mask != 0u
            || inputs.bucket_count > slot_count_from_bytes) {
            throw VulkanRuntimeError(
                "seed lookup sorted-dense index representation is malformed");
        }
    } else {
        const std::uint64_t open_capacity =
            static_cast<std::uint64_t>(inputs.slot_mask) + 1u;
        if (inputs.slot_mask == 0u
            || !std::has_single_bit(open_capacity)
            || open_capacity > slot_count_from_bytes) {
            throw VulkanRuntimeError(
                "seed lookup open-addressed index representation is malformed");
        }
    }
    const SeedAnchorSortPlan expected_sort_plan = make_seed_anchor_sort_plan(
        exact_anchor_count,
        inputs.target_len,
        inputs.target_position_offset,
        gpu_anchor_sort_capacity_,
        features_.max_workgroup_count_x,
        features_.max_workgroup_invocations,
        features_.max_workgroup_size_x,
        features_.max_storage_buffer_range);
    if (!sort_plan.valid() || sort_plan != expected_sort_plan
        || sort_plan.backend == SeedAnchorSortBackend::none) {
        throw VulkanRuntimeError(
            "seed lookup sort plan does not match the runtime/device contract");
    }
    if (prefix_counts.back() != exact_anchor_count) {
        throw VulkanRuntimeError("seed lookup prefix total disagrees with anchor count");
    }
    const std::uint64_t minimizer_count = prefix_counts.size() - 1u;
    constexpr std::uint32_t seed_local = 128u;
    const std::uint64_t seed_groups =
        (minimizer_count + seed_local - 1u) / seed_local;
    if (seed_groups > features_.max_workgroup_count_x) {
        throw VulkanRuntimeError("seed lookup dispatch exceeds maxComputeWorkGroupCount[0]");
    }

    const PhaseBPipeline seed_pipe =
        get_phase_b_pipeline_locked(ShaderKind::seed_lookup_anchors, seed_local, 256u);

    Buffer scratch = create_host_buffer(4u, kPhaseBBufferUsage);

    Buffer uploaded_min_words;
    const Buffer* min_words = inputs.device_minimizer_words;
    if (min_words == nullptr) {
        const std::uint64_t min_bytes = std::max<std::uint64_t>(
            minimizer_device_bytes(inputs.minimizer_word_kind,
                                   inputs.minimizer_words32_per_record, minimizer_count), 4u);
        uploaded_min_words = create_device_buffer(min_bytes, kPhaseBBufferUsage);
        if (uploaded_min_words.mapped != nullptr) {
            std::memset(uploaded_min_words.mapped, 0,
                        static_cast<std::size_t>(uploaded_min_words.bytes));
        }
        if (!inputs.host_minimizer_bytes.empty()) {
            upload_locked(scratch, uploaded_min_words, inputs.host_minimizer_bytes,
                          VK_ACCESS_SHADER_READ_BIT);
        }
        min_words = &uploaded_min_words;
    }

    Buffer prefix_buf = create_device_buffer(
        std::max<std::uint64_t>(prefix_counts.size() * sizeof(std::uint32_t), 4u),
        kPhaseBBufferUsage);
    upload_locked(scratch, prefix_buf, std::as_bytes(prefix_counts), VK_ACCESS_SHADER_READ_BIT);

    const std::uint64_t anchor_bytes = sort_plan.anchor_bytes;
    Buffer anchors_buf = create_device_buffer(anchor_bytes, kPhaseBBufferUsage);
    Buffer anchor_keys_buf = create_device_buffer(
        sort_plan.key_bytes, kPhaseBBufferUsage);
    Buffer ranges_buf;
    std::optional<PhaseBPipeline> sort_pipe;
    Buffer anchors_scratch_buf;
    Buffer anchor_keys_scratch_buf;
    Buffer histogram_buf;
    if (sort_plan.backend == SeedAnchorSortBackend::bitonic) {
        ranges_buf = create_device_buffer(sizeof(SortRangeWire), kPhaseBBufferUsage);
        const SortRangeWire range{
            0u, static_cast<std::uint32_t>(exact_anchor_count)};
        upload_locked(scratch, ranges_buf,
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(&range), sizeof(range)},
            VK_ACCESS_SHADER_READ_BIT);
        sort_pipe = get_phase_b_pipeline_locked(
            ShaderKind::sort_anchors_bitonic,
            gpu_anchor_sort_capacity_,
            256u);
    } else {
        anchors_scratch_buf = create_device_buffer(
            sort_plan.anchor_bytes, kPhaseBBufferUsage);
        anchor_keys_scratch_buf = create_device_buffer(
            sort_plan.key_bytes, kPhaseBBufferUsage);
        histogram_buf = create_device_buffer(
            sort_plan.histogram_bytes, kPhaseBBufferUsage);
    }

    {
        const std::array<VkBuffer, 6> seed_bufs{
            min_words->buffer, inputs.slots->buffer, inputs.positions->buffer,
            prefix_buf.buffer, anchors_buf.buffer, anchor_keys_buf.buffer};
        bind_phase_b_descriptors_locked(seed_pipe, seed_bufs);
        if (sort_plan.backend == SeedAnchorSortBackend::bitonic) {
            const std::array<VkBuffer, 2> sort_bufs{anchors_buf.buffer, ranges_buf.buffer};
            bind_phase_b_descriptors_locked(*sort_pipe, sort_bufs);
        }
    }

    if (sort_plan.backend == SeedAnchorSortBackend::radix) {
        reset_phase_c_pool_locked();
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(seed)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(seed)");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host_to_shader,
                         0, nullptr, 0, nullptr);

    const SeedLookupPush seed_push{
        static_cast<std::uint32_t>(minimizer_count), inputs.minimizer_words32_per_record,
        inputs.minimizer_word_kind, inputs.hash_shift, inputs.pos_bits,
        static_cast<std::uint32_t>(query_len), kmer_size, inputs.slot_mask,
        inputs.bucket_count,
        static_cast<std::uint32_t>(exact_anchor_count),
        sort_plan.target_ref_bits};
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, seed_pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            seed_pipe.pipeline_layout, 0, 1, &seed_pipe.descriptor_set, 0, nullptr);
    vkCmdPushConstants(command_buffer_, seed_pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(seed_push), &seed_push);
    vkCmdDispatch(command_buffer_, static_cast<std::uint32_t>(seed_groups), 1, 1);

    VkMemoryBarrier shader_to_shader{};
    shader_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_shader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_shader.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &shader_to_shader, 0, nullptr, 0, nullptr);

    bool radix_result_in_scratch = false;
    if (sort_plan.backend == SeedAnchorSortBackend::bitonic) {
        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, sort_pipe->pipeline);
        vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                sort_pipe->pipeline_layout, 0, 1, &sort_pipe->descriptor_set, 0, nullptr);
        vkCmdDispatch(command_buffer_, 1, 1, 1);
    } else {
        const GpuArenaSlice anchors{
            &anchors_buf, 0u, sort_plan.anchor_bytes};
        const GpuArenaSlice keys{
            &anchor_keys_buf, 0u, sort_plan.key_bytes};
        const GpuArenaSlice anchors_scratch{
            &anchors_scratch_buf, 0u, sort_plan.anchor_bytes};
        const GpuArenaSlice keys_scratch{
            &anchor_keys_scratch_buf, 0u, sort_plan.key_bytes};
        const GpuArenaSlice histogram{
            &histogram_buf, 0u, sort_plan.histogram_bytes};
        const GpuSortPlan radix_plan{
            exact_anchor_count, 8u, 24u, 8u,
            sort_plan.radix_pass_count};
        record_radix_sort_locked(
            command_buffer_, keys, anchors, keys_scratch, anchors_scratch,
            histogram, radix_plan, sort_plan.num_workgroups,
            sort_plan.elements_per_workgroup, 3u,
            radix_result_in_scratch);
        if (radix_result_in_scratch
            != seed_radix_result_in_scratch(sort_plan.radix_pass_count)) {
            throw VulkanRuntimeError("seed radix sort result parity mismatch");
        }
    }

    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &shader_to_host,
                         0, nullptr, 0, nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(seed)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(seed)");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(seed)");
    require_immutable_metal_policy_live("seed operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(seed)");

    SeedLookupResult result;
    result.count = exact_anchor_count;
    result.seed_lookup_dispatches = 1u;
    result.open_addressed_lookup_dispatches =
        inputs.bucket_count == 0u ? 1u : 0u;
    result.sorted_dense_lookup_dispatches =
        inputs.bucket_count != 0u ? 1u : 0u;
    result.host_bytes.resize(static_cast<std::size_t>(anchor_bytes));
    const Buffer& sorted_anchors = radix_result_in_scratch
        ? anchors_scratch_buf : anchors_buf;
    download_locked(scratch, sorted_anchors, std::span<std::byte>{
        result.host_bytes.data(), result.host_bytes.size()});

    if (sort_plan.backend == SeedAnchorSortBackend::bitonic) {
        result.sort_proof.bitonic_sort_dispatches = 1u;
    } else {
        result.sort_proof.radix_sort_operations = 1u;
        result.sort_proof.radix_anchor_count = exact_anchor_count;
        result.sort_proof.radix_passes = sort_plan.radix_pass_count;
        result.sort_proof.radix_histogram_dispatches =
            sort_plan.radix_pass_count;
        result.sort_proof.radix_scan_dispatches = sort_plan.radix_pass_count;
        result.sort_proof.radix_scatter_dispatches =
            sort_plan.radix_pass_count;
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

// ===========================================================================
// DOC0310 Phase C — arena + frontend implementation
// ===========================================================================

namespace {

constexpr VkBufferUsageFlags kPhaseCArenaUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

// Tile span (tile_bp + overlap) must fit the cooperative sketch's shared
// candidate arrays. The sketch runs one workgroup per tile with this local size;
// all lanes hash candidates in parallel into shared memory.
constexpr std::uint32_t kPhaseCSketchTileBp = 256u;
constexpr std::uint32_t kPhaseCSketchLocal = 64u;
constexpr std::uint32_t kPhaseCSketchTileSpanMax = 384u;
constexpr std::uint32_t kPhaseCScanBlock = 256u;
constexpr std::uint32_t kPhaseCSlotOffsetAlign = 256u;

struct PhaseCSketchBatchArenaRequest {
    bool valid = false;
    std::uint32_t num_blocks = 0;
    std::uint64_t requested_bytes = 0;
};

/// Authoritative eight-slice request shared by admission planning and the
/// dispatch itself: sequence, LUT, tile records, counts, offsets, scan blocks,
/// counters, and pair-value scratch.
[[nodiscard]] PhaseCSketchBatchArenaRequest
phase_c_sketch_batch_arena_request(
    std::uint64_t packed_sequence_bytes,
    std::uint64_t tile_count) noexcept
{
    constexpr std::uint64_t limit =
        (std::numeric_limits<std::uint64_t>::max)();
    if (tile_count == 0u
        || tile_count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
        return {};
    }
    const std::uint32_t num_blocks = std::max<std::uint32_t>(
        phase_c_ceil_div_u32(tile_count, kPhaseCScanBlock),
        1u);
    const auto append_slice = [](
        std::uint64_t total,
        std::uint64_t bytes) noexcept -> std::uint64_t {
        return sat_add(
            total,
            phase_c_align_up(
                std::max<std::uint64_t>(bytes, 4u),
                kPhaseCSlotOffsetAlign));
    };
    const std::uint64_t tile_bytes = phase_c_saturating_mul(
        tile_count,
        sizeof(PhaseCSketchTileWire));
    const std::uint64_t tile_u32_bytes = phase_c_saturating_mul(
        tile_count,
        sizeof(std::uint32_t));
    const std::uint64_t block_bytes = phase_c_saturating_mul(
        num_blocks,
        sizeof(std::uint32_t));
    std::uint64_t requested_bytes = 0u;
    requested_bytes = append_slice(requested_bytes, packed_sequence_bytes);
    requested_bytes = append_slice(requested_bytes, sizeof(VulkanRankLut));
    requested_bytes = append_slice(requested_bytes, tile_bytes);
    requested_bytes = append_slice(requested_bytes, tile_u32_bytes);
    requested_bytes = append_slice(requested_bytes, tile_u32_bytes);
    requested_bytes = append_slice(requested_bytes, block_bytes);
    requested_bytes = append_slice(requested_bytes, 8u);
    requested_bytes = append_slice(requested_bytes, 16u);
    if (tile_bytes == limit || tile_u32_bytes == limit
        || block_bytes == limit || requested_bytes == limit) {
        return {};
    }
    return {
        .valid = true,
        .num_blocks = num_blocks,
        .requested_bytes = requested_bytes,
    };
}

using TileC = PhaseCSketchTileWire;
static_assert(sizeof(TileC) == 24);

struct SketchPushC {
    std::uint32_t emit_mode, window_size, kmer_size, hash_bits, hash_shift, pos_bits;
    std::uint32_t word_kind, words32_per_minimizer, span_value, alphabet_kind;
    std::uint32_t unit_bits, bits_per_char, letters_per_unit, char_mask;
    std::uint32_t tile_count, emit_pairs, query_gate_mask, query_gate_word_count;
};
static_assert(sizeof(SketchPushC) == 72);

struct ScanPushC {
    std::uint32_t element_count, block_size, num_blocks, pass;
};
static_assert(sizeof(ScanPushC) == 16);

struct SeedPushC {
    std::uint32_t words32_per_minimizer, word_kind, hash_shift, pos_bits, kmer_size;
    std::uint32_t slot_mask, max_occ, emit_mode, record_count, bucket_count;
    std::uint32_t target_pos_offset_lo, target_pos_offset_hi, target_ref_bits;
};
static_assert(sizeof(SeedPushC) == 52);

struct PhaseCAnchorSortKeyPlan {
    std::uint32_t target_ref_bits = 63u;
    std::uint32_t pass_count = 8u;
};

[[nodiscard]] PhaseCAnchorSortKeyPlan phase_c_anchor_sort_key_plan(
    std::uint64_t target_len,
    std::uint64_t target_position_offset) noexcept
{
    const auto pass_count = runtime_shape_v5_radix_pass_count(
        target_position_offset, target_len);
    if (!pass_count.has_value()) {
        return PhaseCAnchorSortKeyPlan{};
    }

    const std::uint64_t max_target_coordinate =
        target_position_offset + target_len - 1u;
    const std::uint32_t target_ref_bits = std::max<std::uint32_t>(
        1u, static_cast<std::uint32_t>(std::bit_width(max_target_coordinate)));
    return PhaseCAnchorSortKeyPlan{
        .target_ref_bits = target_ref_bits,
        .pass_count = *pass_count,
    };
}

struct SortPushC {
    std::uint32_t element_count, radix_shift, num_workgroups, elems_per_wg, value_words;
};
static_assert(sizeof(SortPushC) == 20);

struct IndexPushC {
    std::uint32_t element_count, slot_mask, slot_capacity, num_buckets, probe_cap, pass;
};
static_assert(sizeof(IndexPushC) == 24);

struct HostQRec {
    std::uint32_t min_offset, min_count, query_len, flags;
};
static_assert(sizeof(HostQRec) == 16);

/// Full compute→compute memory barrier between dependent Phase C dispatches.
void record_compute_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void record_host_to_shader(VkCommandBuffer cmd) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void record_shader_to_host(VkCommandBuffer cmd) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

[[nodiscard]] std::uint32_t ceil_div_u32(std::uint64_t n, std::uint32_t d) noexcept {
    return static_cast<std::uint32_t>((n + d - 1u) / d);
}

}  // namespace

PhaseCSketchBatchAllocationRequirements
Runtime::phase_c_sketch_batch_allocation_requirements(
    std::uint64_t packed_sequence_bytes,
    std::uint64_t tile_count,
    std::uint32_t minimizer_word_kind,
    std::uint32_t minimizer_words32,
    std::uint64_t max_minimizers)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError(
            "phase_c_sketch_batch_allocation_requirements on unavailable device");
    }
    const bool minimizer_shape_valid =
        (minimizer_word_kind == 0u && minimizer_words32 == 0u)
        || (minimizer_word_kind == 1u && minimizer_words32 == 1u)
        || (minimizer_word_kind == 2u && minimizer_words32 == 2u)
        || (minimizer_word_kind == 3u && minimizer_words32 == 4u);
    const PhaseCSketchBatchArenaRequest arena_request =
        phase_c_sketch_batch_arena_request(
            packed_sequence_bytes,
            tile_count);
    if (!minimizer_shape_valid || !arena_request.valid
        || max_minimizers == 0u
        || max_minimizers
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
        return {};
    }
    const std::uint64_t output_requested_bytes =
        std::max<std::uint64_t>(
            minimizer_device_bytes(
                minimizer_word_kind,
                minimizer_words32,
                max_minimizers),
            4u);
    const std::uint64_t arena_requested_allocation =
        buffer_memory_requirement_locked(
            arena_request.requested_bytes,
            kPhaseCArenaUsage);
    const std::uint64_t current_arena_allocation = phase_c_arena_
        ? phase_c_arena_->device_buffer().allocation_bytes
        : 0u;
    return {
        .valid = true,
        .arena_requested_bytes = arena_request.requested_bytes,
        .arena_allocation_bytes = std::max(
            arena_requested_allocation,
            current_arena_allocation),
        .pessimistic_output_requested_bytes = output_requested_bytes,
        .pessimistic_output_allocation_bytes =
            buffer_memory_requirement_locked(
                output_requested_bytes,
                kPhaseBBufferUsage),
    };
}

Buffer Runtime::run_generate_units_roundtrip(
    std::uint32_t word_bits,
    std::span<const std::uint32_t> cdf,
    GenerateUnitsPush push,
    std::uint64_t out_block_bytes) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_generate_units_roundtrip on unavailable device");
    }

    constexpr std::uint32_t local = 256u;
    const PhaseBPipeline pipe =
        get_phase_b_pipeline_locked(ShaderKind::generate_units, local, word_bits);

    Buffer out = create_device_buffer(
        std::max<std::uint64_t>(out_block_bytes, 4u), kPhaseBBufferUsage);
    Buffer cdf_buf = create_device_buffer(
        std::max<std::uint64_t>(
            static_cast<std::uint64_t>(cdf.size()) * sizeof(std::uint32_t), 4u),
        kPhaseBBufferUsage);
    Buffer scratch = create_host_buffer(4u, kPhaseBBufferUsage);
    upload_locked(scratch, cdf_buf, std::as_bytes(cdf), VK_ACCESS_SHADER_READ_BIT);

    const std::array<VkBuffer, 2> buffers{out.buffer, cdf_buf.buffer};
    bind_phase_b_descriptors_locked(pipe, buffers);

    const std::uint64_t max_groups = features_.max_workgroup_count_x;
    const std::uint64_t tile_chunks = std::max<std::uint64_t>(1, max_groups * local);

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(gen)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(gen)");
    record_host_to_shader(command_buffer_);
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipe.pipeline_layout, 0, 1, &pipe.descriptor_set, 0, nullptr);
    for (std::uint64_t base = 0; base < push.total_chunks; base += tile_chunks) {
        push.chunk_base = static_cast<std::uint32_t>(base);
        const std::uint64_t n =
            std::min<std::uint64_t>(tile_chunks, push.total_chunks - base);
        const std::uint32_t groups =
            static_cast<std::uint32_t>((n + local - 1) / local);
        vkCmdPushConstants(command_buffer_, pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        vkCmdDispatch(command_buffer_, groups, 1, 1);
    }
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(gen)");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(gen)");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(gen)");
    require_immutable_metal_policy_live("generate operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(gen)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(gen-post)");
    if (push.total_chunks != 0) {
        generate_units_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return out;
}

void Runtime::download_slice(
    const Buffer& src,
    std::uint64_t src_byte_offset,
    std::span<std::byte> dst) {
    if (dst.empty()) {
        return;
    }
    if (src.mapped == nullptr) {
        throw VulkanRuntimeError("download_slice: device buffer not host-visible");
    }
    if (src_byte_offset > src.bytes ||
        dst.size() > static_cast<std::size_t>(src.bytes - src_byte_offset)) {
        throw VulkanRuntimeError("download_slice: out of range");
    }
    DEBUG_ASSERT(src_byte_offset <= src.bytes && dst.size() <= src.bytes - src_byte_offset,
                 "download_slice: out of range");
    std::memcpy(
        dst.data(),
        static_cast<const std::byte*>(src.mapped) + src_byte_offset,
        dst.size());
}

// --- Arena -----------------------------------------------------------------

GpuArena& Runtime::phase_c_arena() {
    if (!phase_c_arena_) {
        phase_c_arena_.emplace(*this);
    }
    return *phase_c_arena_;
}

bool GpuArena::ensure_capacity(
    std::uint64_t device_bytes, std::uint64_t upload_bytes, std::uint64_t download_bytes)
{
    // Only called at op start (cursors already at their rewound baseline). Grows
    // a buffer geometrically when needed and resets just that buffer's cursor;
    // never grows mid-op (DOC0310 §7 forbids it while slices are live).
    try {
        if (device_bytes > device_capacity_) {
            // Operation starts only from a rewound mark. Release the obsolete
            // arena before allocating its exact replacement so growth never
            // creates an unadmitted old+new overlap or geometric 2x tail.
            device_.reset();
            device_capacity_ = 0u;
            device_ = runtime_->create_device_buffer(
                device_bytes, kPhaseCArenaUsage);
            if (device_.mapped == nullptr) return false;  // Phase C needs unified memory.
            device_capacity_ = device_.bytes;
            device_cursor_ = 0;
        }
        if (upload_bytes > upload_capacity_) {
            upload_.reset();
            upload_capacity_ = 0u;
            upload_ = runtime_->create_host_buffer(
                upload_bytes,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            upload_capacity_ = upload_.bytes;
            upload_cursor_ = 0;
        }
        if (download_bytes > download_capacity_) {
            download_.reset();
            download_capacity_ = 0u;
            download_ = runtime_->create_host_buffer(
                download_bytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            download_capacity_ = download_.bytes;
            download_cursor_ = 0;
        }
    } catch (const VulkanRuntimeError&) {
        return false;
    }
    return true;
}

bool GpuArena::release_idle_capacity() noexcept {
    if (device_cursor_ != 0u || upload_cursor_ != 0u
        || download_cursor_ != 0u) {
        return false;
    }
    device_.reset();
    upload_.reset();
    download_.reset();
    device_capacity_ = 0u;
    upload_capacity_ = 0u;
    download_capacity_ = 0u;
    return true;
}

GpuArenaSlice GpuArena::allocate_device(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t a = std::max<std::uint64_t>(alignment, kPhaseCSlotOffsetAlign);
    const std::uint64_t offset = align_up(device_cursor_, a);
    if (offset + bytes > device_capacity_) {
        return GpuArenaSlice{};  // caller checks .valid()
    }
    device_cursor_ = offset + bytes;
    return GpuArenaSlice{&device_, offset, bytes};
}

GpuArenaSlice GpuArena::allocate_upload(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t offset = align_up(upload_cursor_, std::max<std::uint64_t>(alignment, 16u));
    if (offset + bytes > upload_capacity_) return GpuArenaSlice{};
    upload_cursor_ = offset + bytes;
    return GpuArenaSlice{&upload_, offset, bytes};
}

GpuArenaSlice GpuArena::allocate_download(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t offset = align_up(download_cursor_, std::max<std::uint64_t>(alignment, 16u));
    if (offset + bytes > download_capacity_) return GpuArenaSlice{};
    download_cursor_ = offset + bytes;
    return GpuArenaSlice{&download_, offset, bytes};
}

void GpuArena::bind_storage_slice(
    VkWriteDescriptorSet& write, VkDescriptorBufferInfo& info,
    std::uint32_t binding, VkDescriptorSet dst_set, const GpuArenaSlice& slice) const noexcept
{
    info = VkDescriptorBufferInfo{slice.buffer->buffer, slice.offset, slice.size};
    write = VkWriteDescriptorSet{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = dst_set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
}

// --- Phase C descriptor pool + dispatch recorder ---------------------------

void Runtime::ensure_phase_c_pool_locked() {
    if (phase_c_pool_ != VK_NULL_HANDLE) return;
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    size.descriptorCount = 4096u * 9u;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = 4096u;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    check_vk(vkCreateDescriptorPool(device_, &info, nullptr, &phase_c_pool_),
             "vkCreateDescriptorPool(phase_c)");
}

void Runtime::reset_phase_c_pool_locked() {
    ensure_phase_c_pool_locked();
    check_vk(vkResetDescriptorPool(device_, phase_c_pool_, 0), "vkResetDescriptorPool(phase_c)");
}

VkDescriptorSet Runtime::alloc_phase_c_set_locked(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = phase_c_pool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    check_vk(vkAllocateDescriptorSets(device_, &info, &set), "vkAllocateDescriptorSets(phase_c)");
    return set;
}

void Runtime::bind_descriptors_offsets_locked(
    const PhaseBPipeline& entry, std::span<const VkDescriptorBufferInfo> infos)
{
    std::vector<VkWriteDescriptorSet> writes(infos.size());
    for (std::size_t b = 0; b < infos.size(); ++b) {
        writes[b] = VkWriteDescriptorSet{};
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = entry.descriptor_set;
        writes[b].dstBinding = static_cast<std::uint32_t>(b);
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void Runtime::record_phase_c_dispatch_locked(
    VkCommandBuffer cmd, const PhaseBPipeline& pipe,
    std::span<const VkDescriptorBufferInfo> infos,
    const void* push, std::uint32_t push_size, std::uint32_t group_count,
    RuntimePipelineUseCollector* collector,
    std::size_t collector_job_index)
{
    if (group_count == 0) return;
    VkDescriptorSet set = alloc_phase_c_set_locked(pipe.set_layout);
    std::vector<VkWriteDescriptorSet> writes(infos.size());
    for (std::size_t b = 0; b < infos.size(); ++b) {
        writes[b] = VkWriteDescriptorSet{};
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = set;
        writes[b].dstBinding = static_cast<std::uint32_t>(b);
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipe.pipeline_layout, 0, 1, &set, 0, nullptr);
    if (push_size > 0 && push != nullptr) {
        vkCmdPushConstants(cmd, pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, push_size, push);
    }
    if (collector != nullptr) {
        collector->use(
            cmd, collector_job_index,
            make_shader_key(
                pipe.kind, 384u, pipe.local_size, pipe.max_span),
            pipe.metal_archive_receipt, group_count);
    }
    vkCmdDispatch(cmd, group_count, 1, 1);
}

void Runtime::record_radix_sort_locked(
    VkCommandBuffer cmd,
    const GpuArenaSlice& keys, const GpuArenaSlice& values,
    const GpuArenaSlice& keys_scratch, const GpuArenaSlice& values_scratch,
    const GpuArenaSlice& histogram,
    const GpuSortPlan& plan, std::uint32_t num_workgroups,
    std::uint32_t elems_per_wg, std::uint32_t value_words,
    bool& result_in_scratch,
    RuntimePipelineUseCollector* collector,
    std::size_t collector_job_index)
{
    const PhaseBPipeline hist_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_sort_radix_histogram, 1, 1);
    const PhaseBPipeline scan_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_sort_radix_scan, 1, 1);
    const PhaseBPipeline scatter_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_sort_radix_scatter, 1, 1);
    if (collector != nullptr) {
        const auto prepare = [collector](const PhaseBPipeline& pipe) {
            collector->prepare(
                make_shader_key(
                    pipe.kind, 384u, pipe.local_size, pipe.max_span),
                pipe.metal_archive_receipt);
        };
        prepare(hist_pipe);
        prepare(scan_pipe);
        prepare(scatter_pipe);
    }

    bool in_scratch = false;
    for (std::uint32_t pass = 0; pass < plan.pass_count; ++pass) {
        const GpuArenaSlice& cur_keys = in_scratch ? keys_scratch : keys;
        const GpuArenaSlice& cur_vals = in_scratch ? values_scratch : values;
        const GpuArenaSlice& out_keys = in_scratch ? keys : keys_scratch;
        const GpuArenaSlice& out_vals = in_scratch ? values : values_scratch;

        SortPushC push{
            static_cast<std::uint32_t>(plan.element_count),
            pass * plan.radix_bits, num_workgroups, elems_per_wg, value_words};

        const std::array<VkDescriptorBufferInfo, 5> infos{{
            {cur_keys.buffer->buffer, cur_keys.offset, cur_keys.size},
            {cur_vals.buffer->buffer, cur_vals.offset, cur_vals.size},
            {out_keys.buffer->buffer, out_keys.offset, out_keys.size},
            {out_vals.buffer->buffer, out_vals.offset, out_vals.size},
            {histogram.buffer->buffer, histogram.offset, histogram.size},
        }};
        record_phase_c_dispatch_locked(
            cmd, hist_pipe, infos, &push, sizeof(push), num_workgroups,
            collector, collector_job_index);
        record_compute_barrier(cmd);
        record_phase_c_dispatch_locked(
            cmd, scan_pipe, infos, &push, sizeof(push), 1,
            collector, collector_job_index);
        record_compute_barrier(cmd);
        record_phase_c_dispatch_locked(
            cmd, scatter_pipe, infos, &push, sizeof(push), num_workgroups,
            collector, collector_job_index);
        record_compute_barrier(cmd);
        in_scratch = !in_scratch;
    }
    result_in_scratch = in_scratch;
}

void Runtime::record_stable_pair_sort_by_value_then_key_locked(
    VkCommandBuffer command_buffer,
    SortedPairDeviceViews& views,
    GpuArenaSlice hist,
    GpuSortPlan value_sort_plan,
    GpuSortPlan key_sort_plan,
    std::uint32_t num_workgroups,
    std::uint32_t elems_per_workgroup)
{
    bool value_sorted_in_scratch = false;
    record_radix_sort_locked(
        command_buffer,
        views.values,
        views.keys,
        views.values_scratch,
        views.keys_scratch,
        hist,
        value_sort_plan,
        num_workgroups,
        elems_per_workgroup,
        1u,
        value_sorted_in_scratch);

    record_compute_barrier(command_buffer);

    const GpuArenaSlice key_after_value_sort =
        value_sorted_in_scratch ? views.keys_scratch : views.keys;
    const GpuArenaSlice value_after_value_sort =
        value_sorted_in_scratch ? views.values_scratch : views.values;
    const GpuArenaSlice key_scratch_after_value_sort =
        value_sorted_in_scratch ? views.keys : views.keys_scratch;
    const GpuArenaSlice value_scratch_after_value_sort =
        value_sorted_in_scratch ? views.values : views.values_scratch;

    bool key_sorted_in_scratch = false;
    record_radix_sort_locked(
        command_buffer,
        key_after_value_sort,
        value_after_value_sort,
        key_scratch_after_value_sort,
        value_scratch_after_value_sort,
        hist,
        key_sort_plan,
        num_workgroups,
        elems_per_workgroup,
        1u,
        key_sorted_in_scratch);

    views.keys = key_after_value_sort;
    views.values = value_after_value_sort;
    views.keys_scratch = key_scratch_after_value_sort;
    views.values_scratch = value_scratch_after_value_sort;
    views.keys_in_scratch = key_sorted_in_scratch;
}

namespace {

/// Pick (num_workgroups, elems_per_wg) for a device sort/scan over `n` elements,
/// keeping the workgroup count under the device dispatch limit.
struct WgPlan { std::uint32_t num_workgroups; std::uint32_t elems_per_wg; };
[[nodiscard]] WgPlan plan_workgroups(std::uint64_t n, std::uint32_t max_wg) {
    if (n == 0) return WgPlan{0u, 1u};
    // Small per-workgroup chunks maximise concurrency for the 1-lane histogram
    // and stable scatter; the parallel scan keeps the per-bin reduction cheap
    // even with many workgroups.
    std::uint32_t elems_per_wg = 512u;
    std::uint32_t num_wg = ceil_div_u32(n, elems_per_wg);
    if (num_wg > max_wg) {
        num_wg = max_wg;
        elems_per_wg = ceil_div_u32(n, num_wg);
    }
    return WgPlan{num_wg, elems_per_wg};
}

/// Next power-of-two open-addressed slot capacity for `distinct` buckets,
/// matching `indexing::gpu_slot_capacity` (load factor < 0.5).
[[nodiscard]] std::uint32_t phase_c_slot_capacity(std::uint64_t distinct) {
    const std::uint64_t wanted = sat_add(bioseq::parallelism::saturating_mul(distinct, 2u), 1u);
    std::uint64_t cap = 2;
    while (cap < wanted) cap <<= 1;
    return static_cast<std::uint32_t>(cap);
}

}  // namespace

RuntimeSketchResult Runtime::run_phase_c_sketch_roundtrip(
    std::span<const std::byte> packed_sequence,
    const VulkanRankLut& rank_lut,
    const VulkanSketchParams& params,
    std::uint64_t max_minimizers,
    std::span<const std::uint32_t> query_gate_words,
    std::uint32_t query_gate_mask)
{
    std::lock_guard<std::mutex> guard(mu_);
    RuntimeSketchResult result;
    if (!features_.available) {
        throw VulkanRuntimeError("run_phase_c_sketch_roundtrip on unavailable device");
    }
    const std::uint32_t bp = params.bp_size;
    const std::uint32_t w = params.window_size;
    const std::uint32_t k = params.kmer_size;
    if (bp == 0u || w == 0u || k == 0u || bp < k) { result.count = 0; return result; }
    if (query_gate_words.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw VulkanRuntimeError("phase_c sketch query gate exceeds uint32 word count");
    }
    const std::uint32_t query_gate_word_count =
        static_cast<std::uint32_t>(query_gate_words.size());
    if ((query_gate_word_count == 0u && query_gate_mask != 0u)
        || (query_gate_word_count != 0u
            && (query_gate_word_count != kPhaseCQueryGateWordCount
                || query_gate_mask != kPhaseCQueryGateMask))) {
        throw VulkanRuntimeError(
            "phase_c sketch query gate has truncated/noncanonical coverage");
    }
    const std::uint64_t query_gate_bytes =
        static_cast<std::uint64_t>(query_gate_words.size()) * sizeof(std::uint32_t);
    const std::uint64_t out_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(
            params.minimizer_word_kind,
            params.minimizer_words32,
            max_minimizers),
        4u);
    const PhaseCSketchMemoryPlan memory_plan = phase_c_sketch_memory_plan(
        bp,
        packed_sequence.size(),
        sizeof(VulkanRankLut),
        max_minimizers,
        out_bytes,
        PhaseCSketchUse::minimizer_words,
        k,
        w,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    if (!memory_plan.valid) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c sketch memory plan invalid (bp={}, max_min={}, lpu={})",
            bp,
            max_minimizers,
            params.letters_per_unit));
    }
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    // DOC0353: the sketch kernel grid-strides one invocation per tile, so the
    // dispatch is ceil(num_tiles / local) workgroups CLAMPED to the device limit
    // (the shader loop covers any tiles beyond the clamped grid). No throw — a
    // window larger than 64 * max_workgroup_count_x tiles is still processed
    // correctly, just over more grid-stride steps.
    const std::uint32_t sketch_groups = std::min(
        ceil_div_u32(num_tiles, kPhaseCSketchLocal),
        features_.max_workgroup_count_x);
    const std::uint32_t overlap = phase_c_replay_overlap_bp(k, w);
    std::vector<TileC> tiles(num_tiles);
    for (std::uint32_t t = 0; t < num_tiles; ++t) {
        const std::uint32_t lo = t * tile_bp;
        const std::uint32_t hi = std::min(bp, (t + 1u) * tile_bp);
        const std::uint32_t rlo = lo > overlap ? lo - overlap : 0u;
        tiles[t] = TileC{params.seq_word_offset, bp, rlo, lo, hi, 0u};
    }
    const std::uint32_t num_blocks = memory_plan.tiles.scan_block_count;
    Buffer out_words = create_device_buffer(out_bytes, kPhaseBBufferUsage);
    if (out_words.mapped == nullptr) throw VulkanRuntimeError("phase_c sketch non-unified memory");

    GpuArena& arena = phase_c_arena();
    const std::uint64_t arena_bytes = sat_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : sat_add(query_gate_bytes, std::uint64_t{kPhaseCSlotOffsetAlign}));
    if (!arena.ensure_capacity(arena_bytes, 0, 0)) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c arena grow failed (bp={}, arena_bytes={})",
            bp,
            arena_bytes));
    }
    const GpuArenaMark mark = arena.mark();

    const GpuArenaSlice seq_s = arena.allocate_device(memory_plan.seq_bytes, 16u);
    const GpuArenaSlice lut_s = arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const GpuArenaSlice tiles_s = arena.allocate_device(memory_plan.tile_bytes, 16u);
    const GpuArenaSlice counts_s =
        arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const GpuArenaSlice offsets_s =
        arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const GpuArenaSlice blocks_s =
        arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const GpuArenaSlice counters_s =
        arena.allocate_device(memory_plan.counter_bytes, 16u);
    const GpuArenaSlice pairvals_s =
        arena.allocate_device(memory_plan.arena_pair_value_bytes, 16u);
    const GpuArenaSlice query_gate_s = query_gate_bytes == 0u
        ? GpuArenaSlice{}
        : arena.allocate_device(query_gate_bytes, 16u);
    if (!seq_s.valid() || !lut_s.valid() || !tiles_s.valid() || !counts_s.valid()
        || !offsets_s.valid() || !blocks_s.valid() || !counters_s.valid()
        || !pairvals_s.valid() || (query_gate_bytes != 0u && !query_gate_s.valid())) {
        arena.rewind(mark);
        throw VulkanRuntimeError(fmt::format(
            "phase_c sketch arena oversub (bp={} arena_bytes={} num_tiles={})",
            bp,
            arena_bytes,
            num_tiles));
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(TileC));
    std::memset(base + counts_s.offset, 0, static_cast<std::size_t>(counts_s.size));
    std::memset(base + counters_s.offset, 0, 8);
    if (query_gate_bytes != 0u) {
        std::memcpy(base + query_gate_s.offset, query_gate_words.data(),
                    static_cast<std::size_t>(query_gate_bytes));
    }

    if (params.minimizer_word_kind == 0u) std::memset(out_words.mapped, 0, static_cast<std::size_t>(out_words.bytes));

    const PhaseBPipeline sketch_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_sketch_tile,
        kPhaseCSketchLocal,
        std::max<std::uint32_t>(w, 1u));
    const PhaseBPipeline scan_block_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scan_add_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);
    reset_phase_c_pool_locked();

    const GpuArenaSlice& gate_binding_s =
        query_gate_bytes == 0u ? counters_s : query_gate_s;
    const std::array<VkDescriptorBufferInfo, 9> sketch_infos{{
        {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
        {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
        {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
        {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        {out_words.buffer, 0, VK_WHOLE_SIZE},
        {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
        {offsets_s.buffer->buffer, offsets_s.offset, offsets_s.size},
        {pairvals_s.buffer->buffer, pairvals_s.offset, pairvals_s.size},
        {gate_binding_s.buffer->buffer, gate_binding_s.offset, gate_binding_s.size},
    }};
    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
        {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        {offsets_s.buffer->buffer, offsets_s.offset, offsets_s.size},
        {blocks_s.buffer->buffer, blocks_s.offset, blocks_s.size},
    }};

    SketchPushC push{
        0u, w, k, params.hash_bits, params.hash_shift, params.pos_bits,
        params.minimizer_word_kind, params.minimizer_words32, k, params.alphabet_kind,
        params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
        num_tiles, 0u, query_gate_mask, query_gate_word_count};
    ScanPushC scan0{num_tiles, kPhaseCScanBlock, num_blocks, 0u};
    ScanPushC scan1{num_tiles, kPhaseCScanBlock, num_blocks, 1u};

    static const auto* _qsketch_d = bioseq::instr::register_region_static("phase_c_query_sketch");
    auto _qsketch_s = bioseq::instr::region(_qsketch_d);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(pc_sketch)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(pc_sketch)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), sketch_groups);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, scan_block_pipe, scan_infos, &scan0, sizeof(scan0), num_blocks);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, scan_add_pipe, scan_infos, &scan0, sizeof(scan0), 1u);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, scan_add_pipe, scan_infos, &scan1, sizeof(scan1), ceil_div_u32(num_tiles, 128u));
    record_compute_barrier(command_buffer_);
    push.emit_mode = 1u;
    record_phase_c_dispatch_locked(command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), sketch_groups);
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(pc_sketch)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(pc_sketch)");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(pc_sketch)");
    require_immutable_metal_policy_live("sketch operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(pc_sketch)");

    std::uint32_t last_off = 0, last_cnt = 0;
    std::memcpy(&last_off, base + offsets_s.offset + std::uint64_t{num_tiles - 1u} * 4u, 4);
    std::memcpy(&last_cnt, base + counts_s.offset + std::uint64_t{num_tiles - 1u} * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(last_off) + last_cnt;

    if (total > max_minimizers) {
        result.overflow = true;
    } else {
        result.count = total;
        result.minimizer_words = std::move(out_words);
    }
    arena.rewind(mark);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(pc_sketch-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

RuntimeSketchBatchResult Runtime::run_phase_c_sketch_tile_batch_roundtrip(
    std::span<const std::byte> packed_sequence,
    const VulkanRankLut& rank_lut,
    const VulkanSketchParams& params,
    std::span<const PhaseCSketchTileWire> tiles,
    std::span<const PhaseCSketchQueryLayoutWire> query_layouts,
    std::uint64_t max_minimizers,
    VulkanSketchHostMaterialization materialization)
{
    std::lock_guard<std::mutex> guard(mu_);
    RuntimeSketchBatchResult result;
    if (!features_.available) {
        throw VulkanRuntimeError(
            "run_phase_c_sketch_tile_batch_roundtrip on unavailable device");
    }
    if (materialization != VulkanSketchHostMaterialization::host_and_device
        && materialization != VulkanSketchHostMaterialization::device_only) {
        throw VulkanRuntimeError(
            "phase_c sketch tile batch has invalid host materialization mode");
    }
    result.dispatch_proof.materialization_ = materialization;
    const std::uint32_t w = params.window_size;
    const std::uint32_t k = params.kmer_size;
    if (tiles.empty() || query_layouts.empty()
        || w == 0u || k == 0u || max_minimizers == 0u) {
        return result;
    }
    if (tiles.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw VulkanRuntimeError("phase_c sketch tile batch exceeds uint32 tile count");
    }
    if (max_minimizers
        > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw VulkanRuntimeError("phase_c sketch tile batch capacity exceeds ABI");
    }
    std::uint64_t layout_tile_cursor = 0u;
    for (const PhaseCSketchQueryLayoutWire& layout : query_layouts) {
        if (layout.begin != layout_tile_cursor
            || layout.count
                > static_cast<std::uint64_t>(tiles.size())
                    - layout_tile_cursor) {
            throw VulkanRuntimeError(
                "phase_c sketch query layout does not densely partition tiles");
        }
        layout_tile_cursor += layout.count;
    }
    if (layout_tile_cursor != tiles.size()) {
        throw VulkanRuntimeError(
            "phase_c sketch query layout does not cover all tiles");
    }

    const std::uint32_t tile_count = static_cast<std::uint32_t>(tiles.size());
    const PhaseCSketchBatchArenaRequest arena_request =
        phase_c_sketch_batch_arena_request(
            packed_sequence.size(),
            tile_count);
    if (!arena_request.valid) {
        throw VulkanRuntimeError(
            "phase_c sketch tile batch arena request is invalid");
    }
    const std::uint32_t num_blocks = arena_request.num_blocks;
    const std::uint32_t sketch_groups = std::min(
        ceil_div_u32(tile_count, kPhaseCSketchLocal),
        features_.max_workgroup_count_x);
    if (sketch_groups == 0u) {
        return result;
    }

    // Preserve the established host-materialized path: it reserves its
    // pessimistic output up front and records count/scan/emit in one submit.
    // Only device-only mode waits for the scan before allocating exact output.
    Buffer host_materialized_out_words;
    if (materialization
        == VulkanSketchHostMaterialization::host_and_device) {
        const std::uint64_t pessimistic_output_bytes =
            std::max<std::uint64_t>(
                minimizer_device_bytes(
                    params.minimizer_word_kind,
                    params.minimizer_words32,
                    max_minimizers),
                4u);
        host_materialized_out_words = create_device_buffer(
            pessimistic_output_bytes,
            kPhaseBBufferUsage);
        if (host_materialized_out_words.mapped == nullptr) {
            throw VulkanRuntimeError(
                "phase_c sketch tile batch non-unified host output memory");
        }
    }

    const std::uint64_t arena_bytes = arena_request.requested_bytes;
    GpuArena& arena = phase_c_arena();
    if (!arena.ensure_capacity(arena_bytes, 0u, 0u)) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c sketch tile batch arena grow failed (tiles={}, arena_bytes={})",
            tile_count,
            arena_bytes));
    }
    const GpuArenaMark mark = arena.mark();
    struct ArenaRewindOnExit {
        GpuArena* arena = nullptr;
        GpuArenaMark mark{};
        ~ArenaRewindOnExit() {
            if (arena != nullptr) arena->rewind(mark);
        }
    } arena_rewind_on_exit{&arena, mark};

    const GpuArenaSlice seq_s = arena.allocate_device(
        std::max<std::uint64_t>(packed_sequence.size(), 4u), 16u);
    const GpuArenaSlice lut_s =
        arena.allocate_device(sizeof(VulkanRankLut), 16u);
    const GpuArenaSlice tiles_s = arena.allocate_device(
        static_cast<std::uint64_t>(tiles.size()) * sizeof(PhaseCSketchTileWire),
        16u);
    const GpuArenaSlice counts_s =
        arena.allocate_device(static_cast<std::uint64_t>(tile_count) * 4u, 16u);
    const GpuArenaSlice offsets_s =
        arena.allocate_device(static_cast<std::uint64_t>(tile_count) * 4u, 16u);
    const GpuArenaSlice blocks_s =
        arena.allocate_device(static_cast<std::uint64_t>(num_blocks) * 4u, 16u);
    const GpuArenaSlice counters_s = arena.allocate_device(8u, 16u);
    const GpuArenaSlice pairvals_s = arena.allocate_device(16u, 16u);
    if (!seq_s.valid() || !lut_s.valid() || !tiles_s.valid()
        || !counts_s.valid() || !offsets_s.valid() || !blocks_s.valid()
        || !counters_s.valid() || !pairvals_s.valid()) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c sketch tile batch arena oversub (tiles={} arena_bytes={})",
            tile_count,
            arena_bytes));
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    std::memset(base + seq_s.offset, 0, static_cast<std::size_t>(seq_s.size));
    if (!packed_sequence.empty()) {
        std::memcpy(
            base + seq_s.offset,
            packed_sequence.data(),
            packed_sequence.size());
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(
        base + tiles_s.offset,
        tiles.data(),
        tiles.size() * sizeof(PhaseCSketchTileWire));
    std::memset(base + counts_s.offset, 0, static_cast<std::size_t>(counts_s.size));
    std::memset(base + offsets_s.offset, 0, static_cast<std::size_t>(offsets_s.size));
    std::memset(base + blocks_s.offset, 0, static_cast<std::size_t>(blocks_s.size));
    std::memset(base + counters_s.offset, 0, static_cast<std::size_t>(counters_s.size));
    if (materialization == VulkanSketchHostMaterialization::host_and_device
        && params.minimizer_word_kind == 0u) {
        std::memset(
            host_materialized_out_words.mapped,
            0,
            static_cast<std::size_t>(host_materialized_out_words.bytes));
    }
    std::unique_ptr<RuntimePipelineUseCollector> query_pipeline_collector;
    const bool causal_pipeline_proof_required =
        materialization == VulkanSketchHostMaterialization::device_only
        && (use_shader_provider_locked()
            || runtime_metal_archive_configuration_installed());
    if (causal_pipeline_proof_required) {
        query_pipeline_collector = RuntimePipelineUseCollector::create(
            RuntimePipelineOperationKind::query_sketch,
            1u,
            next_operation_proof_identity_locked());
    }
    const PhaseBPipeline sketch_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_sketch_tile,
        kPhaseCSketchLocal,
        std::max<std::uint32_t>(w, 1u));
    const PhaseBPipeline scan_block_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_prefix_scan_block,
        1u,
        1u);
    const PhaseBPipeline scan_add_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_prefix_scan_add,
        128u,
        1u);
    if (query_pipeline_collector != nullptr) {
        const auto prepare = [&query_pipeline_collector](
            const PhaseBPipeline& pipe) {
            query_pipeline_collector->prepare(
                make_shader_key(
                    pipe.kind, 384u, pipe.local_size, pipe.max_span),
                pipe.metal_archive_receipt);
        };
        prepare(sketch_pipe);
        prepare(scan_block_pipe);
        prepare(scan_add_pipe);
    }
    reset_phase_c_pool_locked();

    // Device-only count mode never dereferences binding 4, so it can use the
    // tiny counters slice until the exact output exists.  Host mode binds its
    // established pessimistic output, which the same submit later emits into.
    const VkDescriptorBufferInfo initial_output_info =
        materialization == VulkanSketchHostMaterialization::host_and_device
        ? VkDescriptorBufferInfo{
            host_materialized_out_words.buffer, 0, VK_WHOLE_SIZE}
        : VkDescriptorBufferInfo{
            counters_s.buffer->buffer,
            counters_s.offset,
            counters_s.size};
    const std::array<VkDescriptorBufferInfo, 9> count_sketch_infos{{
        {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
        {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
        {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
        {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        initial_output_info,
        {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
        {offsets_s.buffer->buffer, offsets_s.offset, offsets_s.size},
        {pairvals_s.buffer->buffer, pairvals_s.offset, pairvals_s.size},
        {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
    }};
    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
        {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        {offsets_s.buffer->buffer, offsets_s.offset, offsets_s.size},
        {blocks_s.buffer->buffer, blocks_s.offset, blocks_s.size},
    }};

    SketchPushC push{
        0u,
        w,
        k,
        params.hash_bits,
        params.hash_shift,
        params.pos_bits,
        params.minimizer_word_kind,
        params.minimizer_words32,
        k,
        params.alphabet_kind,
        params.unit_bits,
        params.bits_per_char,
        params.letters_per_unit,
        params.char_mask,
        tile_count,
        0u,
        0u,
        0u};
    ScanPushC scan0{tile_count, kPhaseCScanBlock, num_blocks, 0u};
    ScanPushC scan1{tile_count, kPhaseCScanBlock, num_blocks, 1u};

    static const auto* _qsketch_d =
        bioseq::instr::register_region_static("phase_c_query_sketch_batch");
    auto _qsketch_s = bioseq::instr::region(_qsketch_d);
    RuntimeSketchBatchDispatchProof dispatch_proof{};
    dispatch_proof.materialization_ = materialization;
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(pc_sketch_batch)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(
        vkBeginCommandBuffer(command_buffer_, &begin),
        "vkBeginCommandBuffer(pc_sketch_batch_count_scan)");
    if (query_pipeline_collector != nullptr) {
        query_pipeline_collector->begin(command_buffer_);
    }
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, sketch_pipe, count_sketch_infos, &push, sizeof(push),
        sketch_groups, query_pipeline_collector.get(), 0u);
    ++dispatch_proof.count_dispatches_;
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, scan_block_pipe, scan_infos, &scan0, sizeof(scan0),
        num_blocks, query_pipeline_collector.get(), 0u);
    ++dispatch_proof.prefix_scan_dispatches_;
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, scan_add_pipe, scan_infos, &scan0, sizeof(scan0), 1u,
        query_pipeline_collector.get(), 0u);
    ++dispatch_proof.prefix_scan_dispatches_;
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, scan_add_pipe, scan_infos, &scan1, sizeof(scan1),
        ceil_div_u32(tile_count, 128u), query_pipeline_collector.get(), 0u);
    ++dispatch_proof.prefix_scan_dispatches_;
    ++dispatch_proof.prefix_scan_sequences_;
    if (materialization
        == VulkanSketchHostMaterialization::host_and_device) {
        record_compute_barrier(command_buffer_);
        push.emit_mode = 1u;
        record_phase_c_dispatch_locked(
            command_buffer_, sketch_pipe, count_sketch_infos, &push,
            sizeof(push), sketch_groups);
        ++dispatch_proof.emit_dispatches_;
    }
    record_shader_to_host(command_buffer_);
    check_vk(
        vkEndCommandBuffer(command_buffer_),
        "vkEndCommandBuffer(pc_sketch_batch_count_scan)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(
        vkQueueSubmit(queue_, 1, &submit, fence_),
        "vkQueueSubmit(pc_sketch_batch_count_scan)");
    check_vk(
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX),
        "vkWaitForFences(pc_sketch_batch_count_scan)");
    require_immutable_metal_policy_live(
        "batched sketch count operation completion");
    check_vk(
        vkResetFences(device_, 1, &fence_),
        "vkResetFences(pc_sketch_batch_count_scan)");
    if (query_pipeline_collector != nullptr) {
        query_pipeline_collector->commit(command_buffer_);
    }
    ++dispatch_proof.completed_roundtrips_;
    result.dispatch_proof = dispatch_proof;

    result.tile_counts.resize(tiles.size());
    result.tile_offsets.resize(tiles.size());
    std::memcpy(
        result.tile_counts.data(),
        base + counts_s.offset,
        result.tile_counts.size() * sizeof(std::uint32_t));
    std::memcpy(
        result.tile_offsets.data(),
        base + offsets_s.offset,
        result.tile_offsets.size() * sizeof(std::uint32_t));
    const std::uint64_t total =
        static_cast<std::uint64_t>(result.tile_offsets.back())
        + static_cast<std::uint64_t>(result.tile_counts.back());

    if (total > max_minimizers) {
        result.overflow = true;
    } else if (materialization
               == VulkanSketchHostMaterialization::host_and_device) {
        result.count = total;
        const std::uint64_t host_bytes =
            minimizer_host_bytes(params.minimizer_word_kind, total);
        result.host_minimizer_bytes.resize(
            static_cast<std::size_t>(host_bytes));
        if (host_bytes > 0u) {
            std::memcpy(
                result.host_minimizer_bytes.data(),
                host_materialized_out_words.mapped,
                static_cast<std::size_t>(host_bytes));
        }
        result.minimizer_words = std::move(host_materialized_out_words);
    } else {
        // The count/scan result is authoritative.  Allocate only the exact
        // device payload (Vulkan requires a nonzero buffer, hence the 4-byte
        // physical minimum for an empty logical result), then reuse the
        // resident input/count/offset slices for an emit-only submission.
        const std::uint64_t exact_device_bytes = std::max<std::uint64_t>(
            minimizer_device_bytes(
                params.minimizer_word_kind,
                params.minimizer_words32,
                total),
            4u);
        Buffer out_words = create_device_buffer(
            exact_device_bytes,
            kPhaseBBufferUsage);
        if (out_words.mapped == nullptr) {
            throw VulkanRuntimeError(
                "phase_c sketch tile batch non-unified exact output memory");
        }
        if (params.minimizer_word_kind == 0u) {
            std::memset(
                out_words.mapped,
                0,
                static_cast<std::size_t>(out_words.bytes));
        }

        const std::array<VkDescriptorBufferInfo, 9> emit_sketch_infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {out_words.buffer, 0, VK_WHOLE_SIZE},
            {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
            {offsets_s.buffer->buffer, offsets_s.offset, offsets_s.size},
            {pairvals_s.buffer->buffer, pairvals_s.offset, pairvals_s.size},
            {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
        }};

        // The first queue submission is complete, so its descriptor sets may
        // be recycled.  The arena mark and all resident slices remain live.
        reset_phase_c_pool_locked();
        if (query_pipeline_collector != nullptr) {
            query_pipeline_collector->burn_before_reset(command_buffer_);
        }
        check_vk(
            vkResetCommandBuffer(command_buffer_, 0),
            "vkResetCommandBuffer(pc_sketch_batch_emit)");
        check_vk(
            vkBeginCommandBuffer(command_buffer_, &begin),
            "vkBeginCommandBuffer(pc_sketch_batch_emit)");
        if (query_pipeline_collector != nullptr) {
            query_pipeline_collector->begin(command_buffer_);
        }
        // Covers the packed-u16 zero-fill; the compute barrier carries the
        // prior submission's count/offset shader writes into this emit read.
        record_host_to_shader(command_buffer_);
        record_compute_barrier(command_buffer_);
        push.emit_mode = 1u;
        record_phase_c_dispatch_locked(
            command_buffer_, sketch_pipe, emit_sketch_infos, &push,
            sizeof(push), sketch_groups, query_pipeline_collector.get(), 0u);
        ++dispatch_proof.emit_dispatches_;
        record_shader_to_host(command_buffer_);
        check_vk(
            vkEndCommandBuffer(command_buffer_),
            "vkEndCommandBuffer(pc_sketch_batch_emit)");
        check_vk(
            vkQueueSubmit(queue_, 1, &submit, fence_),
            "vkQueueSubmit(pc_sketch_batch_emit)");
        check_vk(
            vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX),
            "vkWaitForFences(pc_sketch_batch_emit)");
        require_immutable_metal_policy_live(
            "batched sketch emit operation completion");
        check_vk(
            vkResetFences(device_, 1, &fence_),
            "vkResetFences(pc_sketch_batch_emit)");
        if (query_pipeline_collector != nullptr) {
            query_pipeline_collector->commit(command_buffer_);
        }
        ++dispatch_proof.completed_roundtrips_;
        result.dispatch_proof = dispatch_proof;

        result.count = total;
        result.minimizer_words = std::move(out_words);
    }
    if (!result.overflow
        && materialization == VulkanSketchHostMaterialization::device_only
        && result.minimizer_words.valid()
        && dispatch_proof.accepted()) {
        if (query_pipeline_collector != nullptr) {
            result.query_authority.pipeline_proof_ =
                query_pipeline_collector->seal();
        }
        result.query_authority.identity_ =
            next_operation_proof_identity_locked();
        result.query_authority.runtime_ = this;
        result.query_authority.buffer_ = result.minimizer_words.buffer;
        result.query_authority.memory_identity_ = vulkan_handle_identity(
            result.minimizer_words.memory);
        result.query_authority.buffer_bytes_ = result.minimizer_words.bytes;
        result.query_authority.allocation_bytes_ =
            result.minimizer_words.allocation_bytes;
        result.query_authority.minimizer_count_ = result.count;
        result.query_authority.window_size_ = params.window_size;
        result.query_authority.kmer_size_ = params.kmer_size;
        result.query_authority.minimizer_word_kind_ =
            params.minimizer_word_kind;
        result.query_authority.minimizer_words32_per_record_ =
            params.minimizer_words32;
        result.query_authority.hash_shift_ = params.hash_shift;
        result.query_authority.pos_bits_ = params.pos_bits;
        result.query_authority.materialization_ = materialization;
        result.query_authority.dispatch_proof_ = dispatch_proof;
        result.query_authority.device_uuid_ = features_.device_uuid;
        result.query_authority.query_records_.reserve(query_layouts.size());
        result.query_authority.query_tile_counts_.reserve(
            query_layouts.size());
        std::uint64_t query_minimizer_cursor = 0u;
        for (const PhaseCSketchQueryLayoutWire& layout : query_layouts) {
            std::uint64_t query_minimizer_count = 0u;
            for (std::uint32_t tile = 0u; tile < layout.count; ++tile) {
                query_minimizer_count +=
                    result.tile_counts[layout.begin + tile];
            }
            const std::uint64_t query_minimizer_offset = layout.count == 0u
                ? query_minimizer_cursor
                : result.tile_offsets[layout.begin];
            if (query_minimizer_offset != query_minimizer_cursor
                || query_minimizer_count
                    > (std::numeric_limits<std::uint32_t>::max)()) {
                throw VulkanRuntimeError(
                    "phase_c sketch query authority partition is invalid");
            }
            result.query_authority.query_records_.push_back(
                PhaseCSeedQueryRecordWire{
                    .minimizer_offset = static_cast<std::uint32_t>(
                        query_minimizer_offset),
                    .minimizer_count = static_cast<std::uint32_t>(
                        query_minimizer_count),
                    .query_len = layout.query_len,
                    .flags = 0u,
                });
            result.query_authority.query_tile_counts_.push_back(
                layout.count);
            query_minimizer_cursor += query_minimizer_count;
        }
        if (query_minimizer_cursor != result.count
            || !result.query_authority.covers_device_output()) {
            throw VulkanRuntimeError(
                "phase_c sketch query authority does not cover device output");
        }
    }
    if (query_pipeline_collector != nullptr) {
        query_pipeline_collector->burn_before_reset(command_buffer_);
    }
    check_vk(
        vkResetCommandBuffer(command_buffer_, 0),
        "vkResetCommandBuffer(pc_sketch_batch-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

namespace {

/// Submit the currently-recorded command buffer and wait. Centralises the
/// begin/end/submit/wait/reset boilerplate for the Phase C multi-submit ops.
void submit_and_wait(VkDevice device, VkQueue queue, VkCommandBuffer cmd, VkFence fence,
                     const char* what,
                     RuntimePipelineUseCollector* collector = nullptr) {
    check_vk(vkEndCommandBuffer(cmd), what);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check_vk(vkQueueSubmit(queue, 1, &submit, fence), what);
    check_vk(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), what);
    require_immutable_metal_policy_live(what);
    check_vk(vkResetFences(device, 1, &fence), what);
    if (collector != nullptr) collector->commit(cmd);
}

}  // namespace

GpuBuiltIndex Runtime::run_phase_c_build_target_index(
    std::span<const std::byte> packed_sequence,
    const VulkanRankLut& rank_lut,
    const VulkanSketchParams& params,
    std::uint64_t max_target_minimizers,
    std::span<const std::uint32_t> query_gate_words,
    std::uint32_t query_gate_mask,
    GpuBuiltIndexGateIdentity query_gate_identity,
    bioseq::parallelism::MemoryPermit* peak_permit)
{
    std::lock_guard<std::mutex> guard(mu_);
    static const auto* _run_d = bioseq::instr::register_region_static("phase_c_index_run");
    auto _run_s = bioseq::instr::region(_run_d);
    GpuBuiltIndex out;
    out.ownership.count_phase_admissions = peak_permit != nullptr ? 1u : 0u;
    if (!features_.available) throw VulkanRuntimeError("run_phase_c_build_target_index on unavailable device");
    const std::uint32_t bp = params.bp_size;
    const std::uint32_t w = params.window_size;
    const std::uint32_t k = params.kmer_size;
    out.target_len = bp;
    out.ownership.target_minimizer_capacity_estimate =
        max_target_minimizers;
    if (bp == 0u || w == 0u || k == 0u || bp < k || max_target_minimizers == 0u) {
        out.failure_reason = GpuBuiltIndexFailureReason::invalid_shape;
        return out;  // empty/invalid shape; selected GPU callers throw.
    }
    if (max_target_minimizers > std::numeric_limits<std::uint32_t>::max()) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::minimizer_capacity_exceeds_u32;
        return out;
    }
    if (query_gate_words.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw VulkanRuntimeError("phase_c index query gate exceeds uint32 word count");
    }
    const std::uint32_t query_gate_word_count =
        static_cast<std::uint32_t>(query_gate_words.size());
    const bool gate_uses_identity_owner = query_gate_identity.canonical_words
        && query_gate_words.data()
            == query_gate_identity.canonical_words->data()
        && query_gate_words.size()
            == query_gate_identity.canonical_words->size();
    const bool gate_shape_matches = query_gate_word_count == 0u
        ? query_gate_identity.well_formed() && !query_gate_identity.enabled
        : query_gate_identity.well_formed()
            && query_gate_identity.enabled
            && query_gate_identity.word_count == query_gate_word_count
            && query_gate_identity.mask == query_gate_mask
            && (gate_uses_identity_owner
                || std::equal(
                    query_gate_words.begin(), query_gate_words.end(),
                    query_gate_identity.canonical_words->begin()));
    if (!gate_shape_matches) {
        throw VulkanRuntimeError(
            "phase_c index query gate lacks matching immutable identity");
    }
    out.gate_identity = std::move(query_gate_identity);
    out.ownership.query_gate_enabled = query_gate_word_count != 0u ? 1u : 0u;
    out.ownership.query_gate_word_count = query_gate_word_count;
    out.ownership.query_gate_mask = query_gate_word_count != 0u
        ? query_gate_mask : 0u;
    out.ownership.query_gate_digest_lo = query_gate_word_count != 0u
        ? out.gate_identity.digest_lo : 0u;
    out.ownership.query_gate_digest_hi = query_gate_word_count != 0u
        ? out.gate_identity.digest_hi : 0u;
    const std::uint64_t query_gate_bytes =
        static_cast<std::uint64_t>(query_gate_words.size()) * sizeof(std::uint32_t);
    const std::uint64_t flag_scan_alignment = std::max<std::uint64_t>(
        kPhaseCSlotOffsetAlign,
        features_.min_storage_buffer_offset_alignment);

    const PhaseCSketchMemoryPlan memory_plan = phase_c_sketch_memory_plan(
        bp,
        packed_sequence.size(),
        sizeof(VulkanRankLut),
        max_target_minimizers,
        0u,
        PhaseCSketchUse::target_index_build,
        k,
        w,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    if (!memory_plan.valid) {
        out.failure_reason = GpuBuiltIndexFailureReason::memory_plan_invalid;
        return out;
    }
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    // DOC0353: grid-strided sketch — clamp the dispatch to the device limit.
    const std::uint32_t sketch_groups = std::min(
        ceil_div_u32(num_tiles, kPhaseCSketchLocal),
        features_.max_workgroup_count_x);
    const std::uint32_t overlap = phase_c_replay_overlap_bp(k, w);
    std::vector<TileC> tiles(num_tiles);
    for (std::uint32_t t = 0; t < num_tiles; ++t) {
        const std::uint32_t lo = t * tile_bp;
        const std::uint32_t hi = std::min(bp, (t + 1u) * tile_bp);
        tiles[t] = TileC{params.seq_word_offset, bp, lo > overlap ? lo - overlap : 0u, lo, hi, 0u};
    }
    // Small per-call scaffolding lives in the shared arena (seq + lut + tiles +
    // tile counts/offsets). The large minimizer/sort/index arrays are sized from
    // the EXACT minimizer count (downloaded after the count pass) as standalone
    // device buffers — never a single 14 GB arena buffer that Metal cannot
    // allocate for the XXL_HIFI 500 Mbp target.
    GpuArena& arena = phase_c_arena();
    GpuArenaTrimGuard arena_trim{arena};
    const std::uint64_t arena_bytes = sat_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : sat_add(query_gate_bytes, std::uint64_t{kPhaseCSlotOffsetAlign}));
    const std::uint64_t count_arena_requirement = std::max(
        arena.device_buffer().allocation_bytes,
        buffer_memory_requirement_locked(arena_bytes, kPhaseCArenaUsage));
    const std::uint64_t count_required_peak = sat_add(
        count_arena_requirement, memory_plan.host_transient_bytes);
    if (peak_permit == nullptr) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission;
        return out;
    }
    if (count_required_peak > peak_permit->bytes()
        && !peak_permit->grow_to(
            count_required_peak, &out.main_peak_admission_failure)) {
        out.failure_reason = GpuBuiltIndexFailureReason::
            count_peak_memory_admission_denied;
        return out;
    }
    if (!arena.ensure_capacity(arena_bytes, 0, 0)) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c index arena grow failed (bp={}, arena_bytes={})",
            bp,
            arena_bytes));
    }
    const GpuArenaMark mark = arena.mark();
    const GpuArenaSlice seq_s = arena.allocate_device(memory_plan.seq_bytes, 16u);
    const GpuArenaSlice lut_s =
        arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const GpuArenaSlice tiles_s =
        arena.allocate_device(memory_plan.tile_bytes, 16u);
    const GpuArenaSlice tcounts_s =
        arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const GpuArenaSlice toffsets_s =
        arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const GpuArenaSlice tblocks_s =
        arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const GpuArenaSlice counters_s =
        arena.allocate_device(memory_plan.counter_bytes, 16u);
    const GpuArenaSlice query_gate_s = query_gate_bytes == 0u
        ? GpuArenaSlice{}
        : arena.allocate_device(query_gate_bytes, 16u);
    if (!seq_s.valid() || !lut_s.valid() || !tiles_s.valid() || !tcounts_s.valid()
        || !toffsets_s.valid() || !tblocks_s.valid() || !counters_s.valid()
        || (query_gate_bytes != 0u && !query_gate_s.valid())) {
        bioseq_log<ERR>("[phase_c index] scaffold arena oversubscribed (bp={}, num_tiles={}, "
                        "arena_bytes={}); returning invalid GPU index",
                        bp, num_tiles, arena_bytes);
        out.failure_reason =
            GpuBuiltIndexFailureReason::device_arena_allocation_failed;
        arena.rewind(mark);
        return out;
    }

    const std::uint64_t count_observed_peak = sat_add(
        arena.device_buffer().allocation_bytes,
        memory_plan.host_transient_bytes);
    if (count_observed_peak > peak_permit->bytes()) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission;
        out.observed_device_peak_bytes =
            arena.device_buffer().allocation_bytes;
        arena.rewind(mark);
        return out;
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(TileC));
    std::memset(base + tcounts_s.offset, 0, static_cast<std::size_t>(tcounts_s.size));
    if (query_gate_bytes != 0u) {
        std::memcpy(base + query_gate_s.offset, query_gate_words.data(),
                    static_cast<std::size_t>(query_gate_bytes));
    }

    const PhaseBPipeline sketch_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_sketch_tile,
        kPhaseCSketchLocal,
        std::max<std::uint32_t>(w, 1u));
    const PhaseBPipeline scan_block_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scan_add_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_prefix_scan_add, 128u, 1u);

    // Count every target build on device first.  Query-gated segment indexes
    // usually retain only a tiny subset, while the ungated 500-Mbp row emits
    // 93,308,896 minimizers from a pessimistic capacity of 150,000,192.  Sizing
    // all four emit/sort streams from that pessimistic capacity made the main
    // peak C*40 bytes (6,000,007,680 bytes) even though the adopted resident
    // index is only exact-T/exact-D.  The count fence makes T authoritative
    // before any large standalone buffer exists.
    std::uint64_t emit_capacity = 0u;
    const GpuArenaSlice& gate_binding_s =
        query_gate_bytes == 0u ? counters_s : query_gate_s;
    {
        {
            const std::array<std::uint32_t, 4> cinit{0u, 0u, 0u, 0u};
            std::memcpy(base + counters_s.offset, cinit.data(), sizeof(cinit));
        }
        {
            static const auto* _rd =
                bioseq::instr::register_region_static("phase_c_target_sketch_count");
            auto _rs = bioseq::instr::region(_rd);
            reset_phase_c_pool_locked();
            const std::array<VkDescriptorBufferInfo, 9> sketch_infos{{
                {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
                {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
                {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
                {tcounts_s.buffer->buffer, tcounts_s.offset, tcounts_s.size},
                {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
                {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
                {toffsets_s.buffer->buffer, toffsets_s.offset, toffsets_s.size},
                {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
                {gate_binding_s.buffer->buffer, gate_binding_s.offset, gate_binding_s.size},
            }};
            const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                {tcounts_s.buffer->buffer, tcounts_s.offset, tcounts_s.size},
                {toffsets_s.buffer->buffer, toffsets_s.offset, toffsets_s.size},
                {tblocks_s.buffer->buffer, tblocks_s.offset, tblocks_s.size},
            }};
            SketchPushC push{0u, w, k, params.hash_bits, params.hash_shift, params.pos_bits,
                params.minimizer_word_kind, params.minimizer_words32, k, params.alphabet_kind,
                params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
                num_tiles, 1u, query_gate_mask, query_gate_word_count};
            const std::uint32_t num_blocks =
                memory_plan.tiles.scan_block_count;
            ScanPushC scan0{
                num_tiles, kPhaseCScanBlock, num_blocks, 0u};
            ScanPushC scan1{
                num_tiles, kPhaseCScanBlock, num_blocks, 1u};
            const std::uint32_t count_scan_block_groups =
                phase_c_grid_stride_dispatch_groups(
                    num_blocks, 1u, features_.max_workgroup_count_x);
            const std::uint32_t count_scan_add_groups =
                phase_c_grid_stride_dispatch_groups(
                    num_tiles, 128u, features_.max_workgroup_count_x);
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_count)");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_count)");
            record_host_to_shader(command_buffer_);
            record_phase_c_dispatch_locked(
                command_buffer_, sketch_pipe, sketch_infos, &push,
                sizeof(push), sketch_groups);
            record_compute_barrier(command_buffer_);
            record_phase_c_dispatch_locked(
                command_buffer_, scan_block_pipe, scan_infos, &scan0,
                sizeof(scan0), count_scan_block_groups);
            record_compute_barrier(command_buffer_);
            record_phase_c_dispatch_locked(
                command_buffer_, scan_add_pipe, scan_infos, &scan0,
                sizeof(scan0), 1u);
            record_compute_barrier(command_buffer_);
            record_phase_c_dispatch_locked(
                command_buffer_, scan_add_pipe, scan_infos, &scan1,
                sizeof(scan1), count_scan_add_groups);
            record_shader_to_host(command_buffer_);
            submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx_count)");
            out.ownership.exact_count_dispatches += 1u;
            out.ownership.exact_count_prefix_scan_dispatches += 3u;
            if (query_gate_bytes != 0u) {
                out.ownership.gate_count_dispatches += 1u;
            }
        }
        std::uint32_t last_offset = 0u;
        std::uint32_t last_count = 0u;
        std::memcpy(
            &last_offset,
            base + toffsets_s.offset
                + std::uint64_t{num_tiles - 1u} * sizeof(std::uint32_t),
            sizeof(last_offset));
        std::memcpy(
            &last_count,
            base + tcounts_s.offset
                + std::uint64_t{num_tiles - 1u} * sizeof(std::uint32_t),
            sizeof(last_count));
        out.ownership.exact_count_control_readbacks += 1u;
        const std::uint64_t exact_count =
            static_cast<std::uint64_t>(last_offset) + last_count;
        const std::uint64_t hard_bound =
            phase_c_target_minimizer_hard_bound(bp, k);
        if ((last_count & 0x80000000u) != 0u
            || exact_count > hard_bound
            || exact_count > static_cast<std::uint64_t>(bp)
            || exact_count
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            bioseq_log<ERR>(
                "[phase_c idx] exact target count={} violates hard bound={} "
                "(capacity estimate={})",
                exact_count, hard_bound, max_target_minimizers);
            out.failure_reason =
                GpuBuiltIndexFailureReason::output_capacity_overflow;
            arena.rewind(mark);
            return out;
        }
        out.ownership.counted_minimizer_count = exact_count;
        out.ownership.capacity_estimate_underestimated =
            exact_count > max_target_minimizers ? 1u : 0u;
        if (exact_count == 0u) {
            const std::uint64_t empty_slot_requirement =
                buffer_memory_requirement_locked(16u, kPhaseBBufferUsage);
            const std::uint64_t empty_position_requirement =
                buffer_memory_requirement_locked(8u, kPhaseBBufferUsage);
            const std::uint64_t empty_peak = sat_add(
                memory_plan.host_transient_bytes,
                sat_add(arena.device_buffer().allocation_bytes,
                    sat_add(empty_slot_requirement,
                            empty_position_requirement)));
            if (peak_permit == nullptr || !peak_permit->grow_to(
                    empty_peak, &out.main_peak_admission_failure)) {
                out.failure_reason = GpuBuiltIndexFailureReason::
                    main_peak_memory_admission_denied;
                arena.rewind(mark);
                return out;
            }
            out.ownership.main_peak_admissions = 1u;
            out.peak_admission_bytes = empty_peak;
            out.ownership.peak_admitted_bytes = empty_peak;
            out.slots = create_device_buffer(16u, kPhaseBBufferUsage);
            out.positions = create_device_buffer(8u, kPhaseBBufferUsage);
            if (out.slots.mapped == nullptr || out.positions.mapped == nullptr
                || out.slots.allocation_bytes != empty_slot_requirement
                || out.positions.allocation_bytes != empty_position_requirement) {
                out.failure_reason =
                    GpuBuiltIndexFailureReason::device_buffer_allocation_failed;
                arena.rewind(mark);
                return out;
            }
            std::memset(out.slots.mapped, 0,
                        static_cast<std::size_t>(out.slots.allocation_bytes));
            std::memset(out.positions.mapped, 0,
                        static_cast<std::size_t>(out.positions.allocation_bytes));
            out.observed_device_peak_bytes = sat_add(
                arena.device_buffer().allocation_bytes,
                sat_add(out.slots.allocation_bytes,
                        out.positions.allocation_bytes));
            out.ownership.peak_observed_bytes = sat_add(
                out.observed_device_peak_bytes,
                memory_plan.host_transient_bytes);
            out.occupied_slots = 0u;
            out.position_count = 0u;
            out.slot_mask = 0u;
            check_vk(
                vkResetCommandBuffer(command_buffer_, 0),
                "reset(pc_idx_empty-post)");
            arena.rewind(mark);
            const bool arena_released = arena_trim.release_now();
            const bool arena_invalid = !arena.device_buffer().valid()
                && !arena.upload_buffer().valid()
                && !arena.download_buffer().valid();
            if (!arena_released || !arena_invalid) {
                out.positions.reset();
                out.slots.reset();
                out.failure_reason =
                    GpuBuiltIndexFailureReason::index_invariant_failed;
                return out;
            }
            out.ownership.verified_emit_count = 0u;
            out.ownership.count_emit_parity_verified = 1u;
            out.ownership.arena_trimmed_before_permit_shrink = 1u;
            out.valid = true;
            return out;
        }
        emit_capacity = exact_count;
    }
    const std::uint64_t emit_flag_bytes = emit_capacity * 4u;
    const std::uint64_t emit_flag_scan_bytes = sat_add(
        phase_c_align_up(emit_flag_bytes, flag_scan_alignment),
        emit_flag_bytes);
    const PhaseCSketchMemoryPlan exact_main_plan = phase_c_sketch_memory_plan(
        bp,
        packed_sequence.size(),
        sizeof(VulkanRankLut),
        emit_capacity,
        0u,
        PhaseCSketchUse::target_index_build,
        k,
        w,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    const auto allocation_requirement = [this](std::uint64_t bytes) {
        return buffer_memory_requirement_locked(bytes, kPhaseBBufferUsage);
    };
    const std::uint64_t initial_key_requirement = allocation_requirement(
        std::max<std::uint64_t>(emit_capacity * 8u, emit_flag_scan_bytes));
    const std::uint64_t initial_value_requirement =
        allocation_requirement(emit_capacity * 8u);
    const std::uint64_t exact_flag_bytes = emit_capacity * 4u;
    const std::uint64_t exact_flag_scan_bytes = sat_add(
        phase_c_align_up(exact_flag_bytes, flag_scan_alignment),
        exact_flag_bytes);
    const std::uint64_t scratch_key_requirement = allocation_requirement(
        std::max<std::uint64_t>(emit_capacity * 8u, exact_flag_scan_bytes));
    const std::uint64_t scratch_value_requirement =
        allocation_requirement(emit_capacity * 8u);
    const WgPlan peak_wg = plan_workgroups(
        emit_capacity, features_.max_workgroup_count_x);
    const std::uint64_t histogram_requirement = allocation_requirement(
        std::uint64_t{peak_wg.num_workgroups} * 256u * 4u);
    const std::uint64_t scan_block_requirement = allocation_requirement(
        std::uint64_t{ceil_div_u32(emit_capacity, kPhaseCScanBlock)} * 4u);
    const std::uint64_t bucket_requirement =
        allocation_requirement(emit_capacity * 16u);
    const std::uint32_t planned_hash_passes = std::clamp<std::uint32_t>(
        (params.hash_bits + 7u) / 8u, 1u, 8u);
    const bool planned_sorted_in_scratch = (planned_hash_passes & 1u) != 0u;
    const std::uint64_t sorted_key_requirement = planned_sorted_in_scratch
        ? scratch_key_requirement : initial_key_requirement;
    const std::uint64_t flag_workspace_requirement = planned_sorted_in_scratch
        ? initial_key_requirement : scratch_key_requirement;
    const std::uint64_t arena_requirement =
        arena.device_buffer().allocation_bytes;
    const std::uint64_t sort_device_peak = sat_add(
        arena_requirement,
        sat_add(initial_key_requirement,
            sat_add(initial_value_requirement,
                sat_add(scratch_key_requirement,
                    sat_add(scratch_value_requirement,
                            histogram_requirement)))));
    const std::uint64_t mark_device_peak = sat_add(
        arena_requirement,
        sat_add(sorted_key_requirement,
            sat_add(scratch_value_requirement,
                sat_add(flag_workspace_requirement,
                        scan_block_requirement))));
    const std::uint64_t bucket_device_peak = sat_add(
        arena_requirement,
        sat_add(sorted_key_requirement,
            sat_add(scratch_value_requirement,
                sat_add(flag_workspace_requirement,
                        bucket_requirement))));
    const std::uint64_t exact_device_peak = std::max(
        sort_device_peak, std::max(mark_device_peak, bucket_device_peak));
    const std::uint64_t exact_main_admission = sat_add(
        exact_device_peak, exact_main_plan.host_transient_bytes);
    if (peak_permit == nullptr || !exact_main_plan.valid
        || exact_main_admission == (std::numeric_limits<std::uint64_t>::max)()) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    if (!peak_permit->grow_to(
            exact_main_admission, &out.main_peak_admission_failure)) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::main_peak_memory_admission_denied;
        arena.rewind(mark);
        return out;
    }
    out.ownership.main_peak_admissions = 1u;
    out.peak_admission_bytes = exact_main_admission;
    out.ownership.peak_admitted_bytes = exact_main_admission;

    Buffer keys = create_device_buffer(
        std::max<std::uint64_t>(emit_capacity * 8u, emit_flag_scan_bytes),
        kPhaseBBufferUsage);   // out_words: u64 hash/min; later flags+scan alias
    Buffer vals = create_device_buffer(emit_capacity * 8u, kPhaseBBufferUsage);   // out_pair_vals: u64 pos_strand/min
    if (keys.mapped == nullptr || vals.mapped == nullptr
        || keys.allocation_bytes != initial_key_requirement
        || vals.allocation_bytes != initial_value_requirement) {
        bioseq_log<ERR>("[phase_c index] capacity buffer alloc failed (cap={}); "
                        "returning invalid GPU index", emit_capacity);
        out.failure_reason =
            GpuBuiltIndexFailureReason::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    const std::uint64_t emit_observed_device = sat_add(
        arena.device_buffer().allocation_bytes,
        sat_add(keys.allocation_bytes, vals.allocation_bytes));
    out.observed_device_peak_bytes = std::max(
        out.observed_device_peak_bytes, emit_observed_device);
    if (sat_add(emit_observed_device,
                exact_main_plan.host_transient_bytes) > peak_permit->bytes()) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const GpuArenaSlice keys_s{&keys, 0, keys.bytes};
    const GpuArenaSlice vals_s{&vals, 0, vals.bytes};
    {
        const std::array<std::uint32_t, 4> cinit{
            0u, static_cast<std::uint32_t>(emit_capacity), 0u, 0u};
        std::memcpy(base + counters_s.offset, cinit.data(), sizeof(cinit));
    }
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_target_sketch");
        auto _rs = bioseq::instr::region(_rd);
        reset_phase_c_pool_locked();
        const std::array<VkDescriptorBufferInfo, 9> infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
            {tcounts_s.buffer->buffer, tcounts_s.offset, tcounts_s.size},
            {keys_s.buffer->buffer, keys_s.offset, keys_s.size},             // out_words
            {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
            {toffsets_s.buffer->buffer, toffsets_s.offset, toffsets_s.size},
            {vals_s.buffer->buffer, vals_s.offset, vals_s.size},             // out_pair_vals
            {gate_binding_s.buffer->buffer, gate_binding_s.offset, gate_binding_s.size},
        }};
        SketchPushC push{1u, w, k, params.hash_bits, params.hash_shift, params.pos_bits,
            params.minimizer_word_kind, params.minimizer_words32, k, params.alphabet_kind,
            params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
            num_tiles, 1u, query_gate_mask, query_gate_word_count};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_emit)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_emit)");
        record_host_to_shader(command_buffer_);
        // Prefix offsets/counts were produced by the prior submit. Establish
        // explicit shader-write -> shader-read visibility before mode 1 reads
        // them; queue order and a host fence alone are not a memory barrier.
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, sketch_pipe, infos, &push, sizeof(push), sketch_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx_emit)");
        out.ownership.deterministic_tile_emit_dispatches += 1u;
        if (query_gate_bytes != 0u) {
            out.ownership.gated_emit_dispatches += 1u;
        } else {
            out.ownership.ungated_emit_dispatches += 1u;
        }
    }
    out.ownership.emit_validation_control_readbacks += 1u;
    std::uint32_t emit_status = 0u;
    std::memcpy(
        &emit_status,
        base + counters_s.offset + 2u * sizeof(std::uint32_t),
        sizeof(emit_status));
    constexpr std::uint32_t kTargetEmitComplete = 0x80000000u;
    constexpr std::uint32_t kTargetEmitCapacityFailure = 1u;
    const auto* tile_offsets = reinterpret_cast<const std::uint32_t*>(
        base + toffsets_s.offset);
    const auto* tile_markers = reinterpret_cast<const std::uint32_t*>(
        base + tcounts_s.offset);
    bool emit_parity_valid = emit_status == 0u
        && tile_offsets[0] == 0u;
    std::uint64_t verified_emit_count = 0u;
    for (std::uint32_t tile = 0u;
         emit_parity_valid && tile < num_tiles;
         ++tile) {
        const std::uint64_t offset = tile_offsets[tile];
        const std::uint64_t next_offset = tile + 1u < num_tiles
            ? static_cast<std::uint64_t>(tile_offsets[tile + 1u])
            : emit_capacity;
        const std::uint32_t marker = tile_markers[tile];
        const std::uint64_t actual = marker & ~kTargetEmitComplete;
        emit_parity_valid = (marker & kTargetEmitComplete) != 0u
            && offset <= next_offset
            && next_offset <= emit_capacity
            && actual == next_offset - offset;
        verified_emit_count = sat_add(verified_emit_count, actual);
    }
    emit_parity_valid = emit_parity_valid
        && verified_emit_count == emit_capacity;
    if (!emit_parity_valid) {
        bioseq_log<ERR>(
            "[phase_c idx] deterministic emit parity failed "
            "(status={}, counted={}, verified={})",
            emit_status, emit_capacity, verified_emit_count);
        out.failure_reason =
            (emit_status & kTargetEmitCapacityFailure) != 0u
                ? GpuBuiltIndexFailureReason::output_capacity_overflow
                : GpuBuiltIndexFailureReason::index_invariant_failed;
        arena.rewind(mark);
        return out;
    }
    const std::uint64_t tm = emit_capacity;
    out.ownership.verified_emit_count = verified_emit_count;
    out.ownership.count_emit_parity_verified = 1u;
    out.ownership.retained_minimizer_count = tm;

    const std::uint64_t tm_flag_bytes = tm * 4u;
    const std::uint64_t tm_flag_scan_bytes = sat_add(
        phase_c_align_up(tm_flag_bytes, flag_scan_alignment),
        tm_flag_bytes);
    const std::uint64_t tm_scratch_key_requirement = allocation_requirement(
        std::max<std::uint64_t>(tm * 8u, tm_flag_scan_bytes));
    const std::uint64_t tm_scratch_value_requirement =
        allocation_requirement(tm * 8u);
    const WgPlan tm_expected_wg = plan_workgroups(
        tm, features_.max_workgroup_count_x);
    const std::uint64_t tm_histogram_requirement = allocation_requirement(
        std::uint64_t{tm_expected_wg.num_workgroups} * 256u * 4u);
    const std::uint64_t tm_scan_block_requirement = allocation_requirement(
        std::uint64_t{ceil_div_u32(tm, kPhaseCScanBlock)} * 4u);

    // Exact-T sort scratch.  Lifetimes are split at GPU fences below so the
    // bucket phase never overlaps the histogram and the unused value buffer.
    const WgPlan swg = plan_workgroups(tm, features_.max_workgroup_count_x);
    const std::uint64_t flag_bytes = tm * 4u;
    const std::uint64_t scanned_offset =
        phase_c_align_up(flag_bytes, flag_scan_alignment);
    const std::uint64_t flag_scan_bytes = sat_add(scanned_offset, flag_bytes);
    Buffer keys_scr, vals_scr, hist;
    {
        static const auto* _alloc_d = bioseq::instr::register_region_static("phase_c_index_alloc");
        auto _alloc_s = bioseq::instr::region(_alloc_d);
        keys_scr = create_device_buffer(
            std::max<std::uint64_t>(tm * 8u, flag_scan_bytes),
            kPhaseBBufferUsage);
        vals_scr = create_device_buffer(tm * 8u, kPhaseBBufferUsage);
        hist = create_device_buffer(std::uint64_t{swg.num_workgroups} * 256u * 4u, kPhaseBBufferUsage);
    }
    if (keys_scr.mapped == nullptr || vals_scr.mapped == nullptr
        || hist.mapped == nullptr
        || keys_scr.allocation_bytes != tm_scratch_key_requirement
        || vals_scr.allocation_bytes != tm_scratch_value_requirement
        || hist.allocation_bytes != tm_histogram_requirement) {
        bioseq_log<ERR>("[phase_c index] scratch buffer alloc failed (tm={}, num_wg={}); "
                        "returning invalid GPU index",
                        tm, swg.num_workgroups);
        out.failure_reason =
            GpuBuiltIndexFailureReason::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    const GpuArenaSlice keys_scr_s{&keys_scr, 0, keys_scr.bytes};
    const GpuArenaSlice vals_scr_s{&vals_scr, 0, vals_scr.bytes};
    const GpuArenaSlice hist_s{&hist, 0, hist.bytes};

    out.observed_device_peak_bytes = sat_add(
        arena.device_buffer().allocation_bytes,
        sat_add(keys.allocation_bytes,
            sat_add(vals.allocation_bytes,
                sat_add(keys_scr.allocation_bytes,
                    sat_add(vals_scr.allocation_bytes,
                            hist.allocation_bytes)))));
    if (sat_add(out.observed_device_peak_bytes,
                exact_main_plan.host_transient_bytes) > peak_permit->bytes()) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }

    // --- Submit 2a: radix sort pairs by hash. ---
    // Only sort the significant hash bytes: for K=15 dna4 the stored hash is
    // ~30 bits → 4 passes, not 8, roughly halving the sort.
    const std::uint32_t hash_passes = std::clamp<std::uint32_t>(
        (params.hash_bits + 7u) / 8u, 1u, 8u);
    GpuSortPlan sort_plan{tm, 8u, 8u, 8u, hash_passes};
    bool sorted_in_scratch = false;
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_index_sort");
        auto _rs = bioseq::instr::region(_rd);
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx2)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx2)");
        record_compute_barrier(command_buffer_);
        record_radix_sort_locked(command_buffer_, keys_s, vals_s, keys_scr_s, vals_scr_s, hist_s,
                                 sort_plan, swg.num_workgroups, swg.elems_per_wg, 1u, sorted_in_scratch);
        if (!sorted_in_scratch) {
            VkMemoryBarrier compute_to_copy{};
            compute_to_copy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            compute_to_copy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            compute_to_copy.dstAccessMask =
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(
                command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 1u, &compute_to_copy,
                0u, nullptr, 0u, nullptr);
            const VkBufferCopy copy{0u, 0u, tm * 8u};
            vkCmdCopyBuffer(
                command_buffer_, vals.buffer, vals_scr.buffer, 1u, &copy);
            VkMemoryBarrier copy_to_host{};
            copy_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            copy_to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            copy_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT
                | VK_ACCESS_HOST_WRITE_BIT;
            vkCmdPipelineBarrier(
                command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0u, 1u, &copy_to_host,
                0u, nullptr, 0u, nullptr);
        }
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx2_sort)");
    }

    // Preserve the sorted value run in its exact-T allocation. Four K=15 hash
    // passes may finish in the source value buffer; copying once here makes the
    // sorted allocation the sole resident position owner and lets the other
    // ping-pong allocation die.
    Buffer positions_exact;
    Buffer flag_scan;
    if (sorted_in_scratch) {
        positions_exact = std::move(vals_scr);
        vals.reset();
        flag_scan = std::move(keys);
    } else {
        positions_exact = std::move(vals_scr);
        vals.reset();
        flag_scan = std::move(keys_scr);
    }
    if (positions_exact.allocation_bytes > tm * 8u) {
        std::memset(
            static_cast<std::byte*>(positions_exact.mapped) + tm * 8u,
            0,
            static_cast<std::size_t>(
                positions_exact.allocation_bytes - tm * 8u));
    }
    hist.reset();

    if (flag_scan.bytes < flag_scan_bytes) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const GpuArenaSlice sorted_keys_s = sorted_in_scratch
        ? GpuArenaSlice{&keys_scr, 0u, tm * 8u}
        : GpuArenaSlice{&keys, 0u, tm * 8u};
    const GpuArenaSlice flags_s{&flag_scan, 0u, flag_bytes};
    const GpuArenaSlice scanned_s{
        &flag_scan, scanned_offset, flag_bytes};
    Buffer iblocks = create_device_buffer(
        std::uint64_t{ceil_div_u32(tm, kPhaseCScanBlock)} * 4u,
        kPhaseBBufferUsage);
    if (iblocks.mapped == nullptr) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    if (iblocks.allocation_bytes != tm_scan_block_requirement) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const GpuArenaSlice iblocks_s{&iblocks, 0u, iblocks.bytes};
    out.observed_device_peak_bytes = std::max(
        out.observed_device_peak_bytes,
        sat_add(arena.device_buffer().allocation_bytes,
            sat_add(sorted_keys_s.buffer->allocation_bytes,
                sat_add(positions_exact.allocation_bytes,
                    sat_add(flag_scan.allocation_bytes,
                            iblocks.allocation_bytes)))));
    if (sat_add(out.observed_device_peak_bytes,
                exact_main_plan.host_transient_bytes) > peak_permit->bytes()) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const VkDescriptorBufferInfo dummy{
        counters_s.buffer->buffer, counters_s.offset, counters_s.size};

    // --- Submit 2b: mark runs and scan flags after sort scratch is released. ---
    {
        static const auto* _rd =
            bioseq::instr::register_region_static("phase_c_index_mark_scan");
        auto _rs = bioseq::instr::region(_rd);
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_mark)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_mark)");
        record_host_to_shader(command_buffer_);
        record_compute_barrier(command_buffer_);
        const PhaseBPipeline mark_pipe = get_phase_b_pipeline_locked(
            ShaderKind::phase_c_index_mark_runs, 128u, 1u);
        const std::array<VkDescriptorBufferInfo, 7> idx_infos{{
            {sorted_keys_s.buffer->buffer, sorted_keys_s.offset, sorted_keys_s.size},
            {flags_s.buffer->buffer, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer, scanned_s.offset, scanned_s.size},
            dummy, dummy, dummy, dummy,
        }};
        IndexPushC ipush{static_cast<std::uint32_t>(tm), 0u, 0u, 0u, 0u, 0u};
        const std::uint32_t mark_groups =
            phase_c_grid_stride_dispatch_groups(
                tm, 128u, features_.max_workgroup_count_x);
        record_phase_c_dispatch_locked(
            command_buffer_, mark_pipe, idx_infos, &ipush, sizeof(ipush),
            mark_groups);
        record_compute_barrier(command_buffer_);
        const PhaseBPipeline scanb = get_phase_b_pipeline_locked(
            ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
        const PhaseBPipeline scana = get_phase_b_pipeline_locked(
            ShaderKind::phase_c_prefix_scan_add, 128u, 1u);
        const std::uint32_t inb = ceil_div_u32(tm, kPhaseCScanBlock);
        const std::array<VkDescriptorBufferInfo, 3> iscan{{
            {flags_s.buffer->buffer, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer, scanned_s.offset, scanned_s.size},
            {iblocks_s.buffer->buffer, iblocks_s.offset, iblocks_s.size},
        }};
        ScanPushC is0{static_cast<std::uint32_t>(tm), kPhaseCScanBlock, inb, 0u};
        ScanPushC is1{static_cast<std::uint32_t>(tm), kPhaseCScanBlock, inb, 1u};
        const std::uint32_t scan_block_groups =
            phase_c_grid_stride_dispatch_groups(
                inb, 1u, features_.max_workgroup_count_x);
        const std::uint32_t scan_add_groups =
            phase_c_grid_stride_dispatch_groups(
                tm, 128u, features_.max_workgroup_count_x);
        record_phase_c_dispatch_locked(
            command_buffer_, scanb, iscan, &is0, sizeof(is0),
            scan_block_groups);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, scana, iscan, &is0, sizeof(is0), 1u);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, scana, iscan, &is1, sizeof(is1),
            scan_add_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx_mark)");
    }
    std::uint32_t scan_last = 0, flag_last = 0;
    std::memcpy(
        &scan_last,
        static_cast<const std::byte*>(flag_scan.mapped)
            + scanned_offset + (tm - 1u) * 4u,
        4u);
    std::memcpy(
        &flag_last,
        static_cast<const std::byte*>(flag_scan.mapped) + (tm - 1u) * 4u,
        4u);
    const std::uint32_t num_buckets = scan_last + flag_last;
    out.ownership.distinct_minimizer_count = num_buckets;
    if (num_buckets == 0u) {
        bioseq_log<ERR>("[phase_c idx] num_buckets=0 (tm={})", tm);
        out.failure_reason = GpuBuiltIndexFailureReason::index_invariant_failed;
        arena.rewind(mark); return out;
    }

    // The dense bucket array is built sorted by hash (the pairs were radix-sorted
    // by hash), so the seed lookup binary-searches it directly — no open-addressed
    // slot table. Building a 134 M-slot table with 50 M atomicCompSwap probes
    // tripped the macOS GPU watchdog (VK_ERROR_DEVICE_LOST) at XXL_HIFI; the
    // sorted-bucket form removes that whole submit. `buckets` becomes the resident
    // index ("slots"); `positions` is the hash-run-ordered pos_strand array.
    iblocks.reset();
    Buffer buckets = create_device_buffer(
        std::uint64_t{num_buckets} * 16u, kPhaseBBufferUsage);
    if (buckets.mapped == nullptr) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    const std::uint64_t exact_bucket_requirement =
        allocation_requirement(std::uint64_t{num_buckets} * 16u);
    if (buckets.allocation_bytes != exact_bucket_requirement) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const GpuArenaSlice buckets_s{&buckets, 0u, buckets.bytes};
    {
        static const auto* _bd = bioseq::instr::register_region_static("phase_c_idx_bucketmem");
        auto _bs = bioseq::instr::region(_bd);
        // Touch the complete allocation, including Vk padding, so resident
        // evidence never fabricates full-touch coverage.
        std::memset(
            buckets.mapped, 0,
            static_cast<std::size_t>(buckets.allocation_bytes));
    }

    out.observed_device_peak_bytes = std::max(
        out.observed_device_peak_bytes,
        sat_add(arena.device_buffer().allocation_bytes,
            sat_add(sorted_keys_s.buffer->allocation_bytes,
                sat_add(positions_exact.allocation_bytes,
                    sat_add(flag_scan.allocation_bytes,
                            buckets.allocation_bytes)))));
    if (sat_add(out.observed_device_peak_bytes,
                exact_main_plan.host_transient_bytes) > peak_permit->bytes()) {
        out.failure_reason =
            GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }

    // --- Submit 3: emit dense sorted buckets (single light dispatch). ---
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_index_slots");
        auto _rs = bioseq::instr::region(_rd);
        const GpuArenaSlice& sorted_keys = sorted_in_scratch ? keys_scr_s : keys_s;
        const std::array<VkDescriptorBufferInfo, 7> idx_infos{{
            {sorted_keys.buffer->buffer, sorted_keys.offset, sorted_keys.size},
            {flags_s.buffer->buffer, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer, scanned_s.offset, scanned_s.size},
            {buckets_s.buffer->buffer, buckets_s.offset, buckets_s.size},
            dummy, dummy, dummy,
        }};
        IndexPushC ipush{static_cast<std::uint32_t>(tm), 0u, 0u, num_buckets, 0u, 0u};
        reset_phase_c_pool_locked();
        const PhaseBPipeline emit_b = get_phase_b_pipeline_locked(ShaderKind::phase_c_index_emit_buckets, 128u, 1u);
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx3)");
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx3)");
        record_host_to_shader(command_buffer_);
        record_compute_barrier(command_buffer_);
        const std::uint32_t bucket_groups =
            phase_c_grid_stride_dispatch_groups(
                tm, 128u, features_.max_workgroup_count_x);
        record_phase_c_dispatch_locked(
            command_buffer_, emit_b, idx_infos, &ipush, sizeof(ipush),
            bucket_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx3_buckets)");
    }

    out.slots = std::move(buckets);   // exact-D sorted dense buckets
    out.positions = std::move(positions_exact);  // exact-T sorted pos_strand
    out.slot_mask = 0u;               // 0 ⇒ seed binary-searches `occupied_slots` buckets
    out.occupied_slots = num_buckets;
    out.position_count = static_cast<std::uint32_t>(tm);
    out.ownership.peak_observed_bytes = sat_add(
        out.observed_device_peak_bytes,
        exact_main_plan.host_transient_bytes);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx-post)");
    arena.rewind(mark);
    const bool arena_released = arena_trim.release_now();
    const bool arena_invalid = !arena.device_buffer().valid()
        && !arena.upload_buffer().valid()
        && !arena.download_buffer().valid();
    if (!arena_released || !arena_invalid) {
        out.positions.reset();
        out.slots.reset();
        out.failure_reason =
            GpuBuiltIndexFailureReason::index_invariant_failed;
        return out;
    }
    out.ownership.arena_trimmed_before_permit_shrink = 1u;
    out.valid = true;
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

GpuSortedPairRun Runtime::run_phase_c_sketch_sort_pairs_roundtrip(
    std::span<const std::byte> packed_sequence,
    const VulkanRankLut& rank_lut,
    const VulkanSketchParams& params,
    std::uint64_t max_target_minimizers)
{
    std::lock_guard<std::mutex> guard(mu_);
    static const auto* _run_d =
        bioseq::instr::register_region_static("phase_c_sorted_pair_run");
    auto _run_s = bioseq::instr::region(_run_d);

    GpuSortedPairRun out;
    if (!features_.available) {
        throw VulkanRuntimeError(
            "run_phase_c_sketch_sort_pairs_roundtrip on unavailable device");
    }

    const std::uint32_t bp = params.bp_size;
    const std::uint32_t window_size = params.window_size;
    const std::uint32_t kmer_size = params.kmer_size;
    out.target_len = bp;
    if (bp == 0u || window_size == 0u || kmer_size == 0u || bp < kmer_size
        || max_target_minimizers == 0u) {
        return out;
    }
    if (max_target_minimizers
        > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return out;
    }

    const PhaseCSketchMemoryPlan memory_plan = phase_c_sketch_memory_plan(
        bp,
        packed_sequence.size(),
        sizeof(VulkanRankLut),
        max_target_minimizers,
        0u,
        PhaseCSketchUse::sorted_pairs,
        kmer_size,
        window_size,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    if (!memory_plan.valid) {
        return out;
    }
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    if (num_tiles == 0u) {
        return out;
    }
    // DOC0353: grid-strided sketch — clamp the dispatch to the device limit.
    const std::uint32_t sketch_groups = std::min(
        ceil_div_u32(num_tiles, kPhaseCSketchLocal),
        features_.max_workgroup_count_x);

    const std::uint32_t overlap =
        phase_c_replay_overlap_bp(kmer_size, window_size);
    std::vector<TileC> tiles(num_tiles);
    for (std::uint32_t tile = 0; tile < num_tiles; ++tile) {
        const std::uint32_t lo = tile * tile_bp;
        const std::uint32_t hi = std::min(bp, (tile + 1u) * tile_bp);
        tiles[tile] = TileC{
            params.seq_word_offset,
            bp,
            lo > overlap ? lo - overlap : 0u,
            lo,
            hi,
            0u};
    }

    GpuArena& arena = phase_c_arena();
    if (!arena.ensure_capacity(memory_plan.arena_scaffold_bytes, 0u, 0u)) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c sorted-pair arena grow failed (bp={}, arena_bytes={})",
            bp,
            memory_plan.arena_scaffold_bytes));
    }

    const GpuArenaMark mark = arena.mark();
    const GpuArenaSlice seq_s = arena.allocate_device(memory_plan.seq_bytes, 16u);
    const GpuArenaSlice lut_s =
        arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const GpuArenaSlice tiles_s =
        arena.allocate_device(memory_plan.tile_bytes, 16u);
    const GpuArenaSlice tcounts_s =
        arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const GpuArenaSlice toffsets_s =
        arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const GpuArenaSlice tblocks_s =
        arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const GpuArenaSlice counters_s =
        arena.allocate_device(memory_plan.counter_bytes, 16u);
    if (!seq_s.valid() || !lut_s.valid() || !tiles_s.valid()
        || !tcounts_s.valid() || !toffsets_s.valid() || !tblocks_s.valid()
        || !counters_s.valid()) {
        arena.rewind(mark);
        return out;
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) {
        std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(TileC));
    std::memset(base + tcounts_s.offset, 0, static_cast<std::size_t>(tcounts_s.size));

    const PhaseBPipeline sketch_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_sketch_tile,
        kPhaseCSketchLocal,
        std::max<std::uint32_t>(window_size, 1u));

    const std::uint64_t capacity = max_target_minimizers;
    Buffer keys = create_device_buffer(capacity * 8u, kPhaseBBufferUsage);
    Buffer values = create_device_buffer(capacity * 8u, kPhaseBBufferUsage);
    if (keys.mapped == nullptr || values.mapped == nullptr) {
        arena.rewind(mark);
        return out;
    }

    const GpuArenaSlice keys_s{&keys, 0u, keys.bytes};
    const GpuArenaSlice values_s{&values, 0u, values.bytes};
    {
        const std::array<std::uint32_t, 4> counters_init{
            0u,
            static_cast<std::uint32_t>(capacity),
            0u,
            0u};
        std::memcpy(base + counters_s.offset, counters_init.data(), sizeof(counters_init));
    }

    {
        static const auto* _sketch_d =
            bioseq::instr::register_region_static("phase_c_sorted_pair_sketch");
        auto _sketch_s = bioseq::instr::region(_sketch_d);
        reset_phase_c_pool_locked();
        const std::array<VkDescriptorBufferInfo, 9> infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
            {tcounts_s.buffer->buffer, tcounts_s.offset, tcounts_s.size},
            {keys_s.buffer->buffer, keys_s.offset, keys_s.size},
            {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
            {toffsets_s.buffer->buffer, toffsets_s.offset, toffsets_s.size},
            {values_s.buffer->buffer, values_s.offset, values_s.size},
            {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
        }};
        SketchPushC push{
            2u,
            window_size,
            kmer_size,
            params.hash_bits,
            params.hash_shift,
            params.pos_bits,
            params.minimizer_word_kind,
            params.minimizer_words32,
            kmer_size,
            params.alphabet_kind,
            params.unit_bits,
            params.bits_per_char,
            params.letters_per_unit,
            params.char_mask,
            num_tiles,
            1u,
            0u,
            0u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_sorted_pair_emit)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_sorted_pair_emit)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, sketch_pipe, infos, &push, sizeof(push),
            sketch_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(
            device_, queue_, command_buffer_, fence_, "submit(pc_sorted_pair_emit)");
    }

    std::uint32_t pair_count32 = 0;
    std::memcpy(&pair_count32, base + counters_s.offset, sizeof(pair_count32));
    const std::uint64_t pair_count = pair_count32;
    // DOC0345 — a window with zero minimizers (e.g. an all-N region) is a
    // LEGITIMATE empty result, not a GPU failure: return valid + empty so the
    // caller emits no pairs rather than throwing VulkanRuntimeError. Only an
    // actual capacity overshoot (count > the worst-case bound) is a real decline.
    if (pair_count == 0u) {
        out.pair_count = 0u;
        out.valid = true;
        arena.rewind(mark);
        return out;
    }
    if (pair_count > max_target_minimizers) {
        arena.rewind(mark);
        return out;
    }

    const WgPlan sort_wg = plan_workgroups(pair_count, features_.max_workgroup_count_x);
    Buffer keys_scratch = create_device_buffer(pair_count * 8u, kPhaseBBufferUsage);
    Buffer values_scratch = create_device_buffer(pair_count * 8u, kPhaseBBufferUsage);
    Buffer histogram = create_device_buffer(
        std::uint64_t{sort_wg.num_workgroups} * 256u * 4u,
        kPhaseBBufferUsage);
    if (keys_scratch.mapped == nullptr || values_scratch.mapped == nullptr
        || histogram.mapped == nullptr) {
        arena.rewind(mark);
        return out;
    }

    SortedPairDeviceViews views{
        .keys = keys_s,
        .values = values_s,
        .keys_scratch = GpuArenaSlice{&keys_scratch, 0u, keys_scratch.bytes},
        .values_scratch = GpuArenaSlice{&values_scratch, 0u, values_scratch.bytes},
        .keys_in_scratch = false,
    };
    const GpuArenaSlice histogram_s{&histogram, 0u, histogram.bytes};
    const GpuSortPlan value_sort_plan{
        .element_count = pair_count,
        .key_bytes = 8u,
        .value_bytes = 8u,
        .radix_bits = 8u,
        .pass_count = 8u,
    };
    const std::uint32_t hash_passes = std::clamp<std::uint32_t>(
        (params.hash_bits + 7u) / 8u,
        1u,
        8u);
    const GpuSortPlan key_sort_plan{
        .element_count = pair_count,
        .key_bytes = 8u,
        .value_bytes = 8u,
        .radix_bits = 8u,
        .pass_count = hash_passes,
    };

    {
        static const auto* _sort_d =
            bioseq::instr::register_region_static("phase_c_sorted_pair_sort");
        auto _sort_s = bioseq::instr::region(_sort_d);
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_sorted_pair_sort)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_sorted_pair_sort)");
        record_stable_pair_sort_by_value_then_key_locked(
            command_buffer_,
            views,
            histogram_s,
            value_sort_plan,
            key_sort_plan,
            sort_wg.num_workgroups,
            sort_wg.elems_per_wg);
        record_shader_to_host(command_buffer_);
        submit_and_wait(
            device_, queue_, command_buffer_, fence_, "submit(pc_sorted_pair_sort)");
    }

    const GpuArenaSlice& sorted_keys = choose_sorted_keys(views);
    const GpuArenaSlice& sorted_values = choose_sorted_values(views);
    const std::uint64_t host_bytes = pair_count * sizeof(std::uint64_t);
    out.host_key_bytes.resize(static_cast<std::size_t>(host_bytes));
    out.host_value_bytes.resize(static_cast<std::size_t>(host_bytes));
    std::memcpy(
        out.host_key_bytes.data(),
        static_cast<const std::byte*>(sorted_keys.buffer->mapped) + sorted_keys.offset,
        static_cast<std::size_t>(host_bytes));
    std::memcpy(
        out.host_value_bytes.data(),
        static_cast<const std::byte*>(sorted_values.buffer->mapped) + sorted_values.offset,
        static_cast<std::size_t>(host_bytes));

    const bool sorted_keys_are_scratch = sorted_keys.buffer == &keys_scratch;
    const bool sorted_values_are_scratch = sorted_values.buffer == &values_scratch;
    out.keys = sorted_keys_are_scratch ? std::move(keys_scratch) : std::move(keys);
    out.values =
        sorted_values_are_scratch ? std::move(values_scratch) : std::move(values);
    out.pair_count = pair_count32;
    out.valid = true;
    arena.rewind(mark);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_sorted_pair_post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// DOC0346 — GPU sequence sort. Two submits (modelled on the index build):
//   1: upload seq+descs -> extract 64-bit canonical key per row -> radix sort.
//   host: build equal-key run ranges; seed `index` from the sorted row ids.
//   2: per-run bitonic tie-break over the full canonical compare; if any run is
//      oversized, continue in the same submit with global full-compare bitonic
//      stages over the padded permutation. Then download the permutation.
std::vector<std::uint32_t> Runtime::run_sequence_sort_roundtrip(
    std::span<const std::byte> packed_units,
    std::span<const SeqDescWire> descs,
    const SeqSortKernelParams& params)
{
    const SeqSortUploadChunk chunk{
        packed_units.data(),
        static_cast<std::uint64_t>(packed_units.size()),
        static_cast<std::uint64_t>(packed_units.size())};
    return run_sequence_sort_roundtrip(std::span<const SeqSortUploadChunk>(&chunk, 1u),
                                       descs, params);
}

std::vector<std::uint32_t> Runtime::run_sequence_sort_roundtrip(
    std::span<const SeqSortUploadChunk> chunks,
    std::span<const SeqDescWire> descs,
    const SeqSortKernelParams& params)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_sequence_sort_roundtrip on unavailable device");
    }
    const std::uint32_t n = static_cast<std::uint32_t>(descs.size());
    if (n == 0) return {};
    if (n == 1) return std::vector<std::uint32_t>{0u};

    std::uint64_t packed_unit_bytes = 0;
    for (const SeqSortUploadChunk& chunk : chunks) {
        if (chunk.byte_size > chunk.padded_byte_size) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: invalid upload chunk padding");
        }
        if (chunk.byte_size > 0u && chunk.bytes == nullptr) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: non-empty upload chunk has null data");
        }
        if (packed_unit_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - chunk.padded_byte_size) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: upload chunks exceed uint64");
        }
        packed_unit_bytes += chunk.padded_byte_size;
    }
    seq_sort_uploaded_bytes_.fetch_add(
        packed_unit_bytes,
        std::memory_order_relaxed);

    static constexpr std::uint32_t kSeqSortLocal = 256u;
    const bool dna4 =
        params.alphabet_kind == static_cast<std::uint32_t>(AlphabetKernelKind::dna4_nt4);
    const ShaderKind key_kind = dna4 ? ShaderKind::seq_sort_extract_key_dna4
                                     : ShaderKind::seq_sort_extract_key_generic;
    const ShaderKind tb_kind  = dna4 ? ShaderKind::seq_sort_tiebreak_dna4
                                     : ShaderKind::seq_sort_tiebreak_generic;
    const ShaderKind gc_kind  = dna4 ? ShaderKind::seq_sort_global_compare_dna4
                                     : ShaderKind::seq_sort_global_compare_generic;
    const ShaderKind dense_kind = dna4 ? ShaderKind::seq_sort_dense_scatter_dna4
                                       : ShaderKind::seq_sort_dense_scatter_generic;
    const ShaderKind prefix_kind = dna4 ? ShaderKind::seq_sort_prefix_check_dna4
                                        : ShaderKind::seq_sort_prefix_check_generic;
    const PhaseBPipeline key_pipe = get_phase_b_pipeline_locked(key_kind, kSeqSortLocal, 0u);
    const PhaseBPipeline tb_pipe  = get_phase_b_pipeline_locked(tb_kind, kSeqSortLocal, 0u);
    const PhaseBPipeline gc_pipe  = get_phase_b_pipeline_locked(gc_kind, kSeqSortLocal, 0u);
    const PhaseBPipeline dense_pipe = get_phase_b_pipeline_locked(dense_kind, kSeqSortLocal, 0u);
    const PhaseBPipeline prefix_pipe = get_phase_b_pipeline_locked(prefix_kind, kSeqSortLocal, 0u);
    const WgPlan swg = plan_workgroups(n, features_.max_workgroup_count_x);
    auto next_power_of_two = [](std::uint32_t value) -> std::uint32_t {
        if (value <= 1u) return 1u;
        if (value > 0x80000000u) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: batch too large for padded bitonic index");
        }
        value -= 1u;
        value |= value >> 1u;
        value |= value >> 2u;
        value |= value >> 4u;
        value |= value >> 8u;
        value |= value >> 16u;
        return value + 1u;
    };
    const std::uint32_t padded_n = next_power_of_two(n);

    // Device buffers (unified/host-coherent on Apple). seq is the concatenated,
    // 4-byte-aligned per-row unit blocks viewed as `uint seq_words[]`.
    const std::uint64_t seq_bytes = std::max<std::uint64_t>(packed_unit_bytes, 4u);
    Buffer seq        = create_device_buffer(seq_bytes, kPhaseBBufferUsage);
    Buffer descs_buf  = create_device_buffer(std::uint64_t{n} * 16u, kPhaseBBufferUsage);
    Buffer keys       = create_device_buffer(std::uint64_t{n} * 8u, kPhaseBBufferUsage);
    Buffer vals       = create_device_buffer(std::uint64_t{n} * 8u, kPhaseBBufferUsage);
    Buffer keys_scr   = create_device_buffer(std::uint64_t{n} * 8u, kPhaseBBufferUsage);
    Buffer vals_scr   = create_device_buffer(std::uint64_t{n} * 8u, kPhaseBBufferUsage);
    Buffer hist       = create_device_buffer(std::uint64_t{swg.num_workgroups} * 256u * 4u, kPhaseBBufferUsage);
    Buffer index_buf  = create_device_buffer(std::uint64_t{padded_n} * 4u, kPhaseBBufferUsage);
    Buffer dense_status = create_device_buffer(4u, kPhaseBBufferUsage);
    if (seq.mapped == nullptr || descs_buf.mapped == nullptr || keys.mapped == nullptr
        || vals.mapped == nullptr || keys_scr.mapped == nullptr || vals_scr.mapped == nullptr
        || hist.mapped == nullptr || index_buf.mapped == nullptr
        || dense_status.mapped == nullptr) {
        throw VulkanRuntimeError("run_sequence_sort_roundtrip: device buffer alloc failed");
    }
    auto* seq_base = static_cast<std::byte*>(seq.mapped);
    std::uint64_t write_offset = 0;
    for (const SeqSortUploadChunk& chunk : chunks) {
        if (chunk.byte_size >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: upload chunk exceeds size_t");
        }
        if (chunk.padded_byte_size - chunk.byte_size >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: upload padding exceeds size_t");
        }
        const auto* bytes = static_cast<const std::byte*>(chunk.bytes);
        const std::size_t byte_count = static_cast<std::size_t>(chunk.byte_size);
        if (byte_count > 0u)
            std::memcpy(seq_base + write_offset, bytes, byte_count);
        write_offset += chunk.byte_size;
        const std::size_t pad_count =
            static_cast<std::size_t>(chunk.padded_byte_size - chunk.byte_size);
        if (pad_count > 0u)
            std::memset(seq_base + write_offset, 0, pad_count);
        write_offset += chunk.padded_byte_size - chunk.byte_size;
    }
    if (write_offset < seq_bytes)
        std::memset(seq_base + write_offset, 0, static_cast<std::size_t>(seq_bytes - write_offset));
    std::memcpy(descs_buf.mapped, descs.data(), descs.size() * sizeof(SeqDescWire));

    const GpuArenaSlice seq_s{&seq, 0, seq.bytes};
    const GpuArenaSlice descs_s{&descs_buf, 0, descs_buf.bytes};
    const GpuArenaSlice keys_s{&keys, 0, keys.bytes};
    const GpuArenaSlice vals_s{&vals, 0, vals.bytes};
    const GpuArenaSlice keys_scr_s{&keys_scr, 0, keys_scr.bytes};
    const GpuArenaSlice vals_scr_s{&vals_scr, 0, vals_scr.bytes};
    const GpuArenaSlice hist_s{&hist, 0, hist.bytes};
    const GpuArenaSlice index_s{&index_buf, 0, index_buf.bytes};
    const GpuArenaSlice dense_status_s{&dense_status, 0, dense_status.bytes};

    auto bump = [this]() { seq_sort_dispatch_count_.fetch_add(1, std::memory_order_relaxed); };

    // Dense-rank GPU fast path. Offset 0 handles short/front-key batches. The
    // tail offset handles long fixed-length common-prefix batches only when the
    // shader proves the prefix identity and the selected key is [0,n)-dense.
    auto* index_ptr = static_cast<std::uint32_t*>(index_buf.mapped);
    auto* dense_status_ptr = static_cast<std::uint32_t*>(dense_status.mapped);
    const std::uint32_t units_for_64 =
        std::max<std::uint32_t>(1u, (64u + params.unit_bits - 1u) / params.unit_bits);
    std::array<std::uint32_t, 2> dense_offsets{0u, 0u};
    std::uint32_t dense_offset_count = 1u;
    if (descs.front().unit_count > units_for_64) {
        const std::uint32_t tail_offset = descs.front().unit_count - units_for_64;
        if (tail_offset != 0u) dense_offsets[dense_offset_count++] = tail_offset;
    }
    for (std::uint32_t attempt = 0; attempt < dense_offset_count; ++attempt) {
        const std::uint32_t key_offset = dense_offsets[attempt];
        const std::uint64_t prefix_work =
            static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(key_offset);
        if (prefix_work > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            continue;
        }
        std::fill(index_ptr, index_ptr + padded_n, 0xffffffffu);
        *dense_status_ptr = 0u;
        const SeqSortPush dense_push{
            n, params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
            static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
            static_cast<std::uint32_t>(params.used_storage_mask >> 32),
            key_offset, key_offset == 0u ? 0u : 1u, 0u};
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort-dense)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort-dense)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> dense_infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer, descs_s.offset, descs_s.size},
            {index_s.buffer->buffer, index_s.offset, index_s.size},
            {dense_status_s.buffer->buffer, dense_status_s.offset, dense_status_s.size},
        }};
        if (key_offset != 0u) {
            record_phase_c_dispatch_locked(
                command_buffer_, prefix_pipe, dense_infos, &dense_push, sizeof(dense_push),
                ceil_div_u32(static_cast<std::uint32_t>(prefix_work), kSeqSortLocal));
            record_compute_barrier(command_buffer_);
        }
        record_phase_c_dispatch_locked(command_buffer_, dense_pipe, dense_infos,
                                       &dense_push, sizeof(dense_push),
                                       ceil_div_u32(n, kSeqSortLocal));
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(seqsort-dense)");
        if (*dense_status_ptr == 0u
            && std::find(index_ptr, index_ptr + n, 0xffffffffu) == index_ptr + n) {
            std::vector<std::uint32_t> perm(index_ptr, index_ptr + n);
            seq_sort_dense_scatter_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
            bump();
            return perm;
        }
    }

    const SeqSortPush key_push{
        n, params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
        static_cast<std::uint32_t>(params.used_storage_mask >> 32), 0u, 0u, 0u};

    // --- Submit 1: extract keys, then radix sort (key,value=row index) ---
    bool sorted_in_scratch = false;
    {
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort1)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort1)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> key_infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer, descs_s.offset, descs_s.size},
            {keys_s.buffer->buffer, keys_s.offset, keys_s.size},
            {vals_s.buffer->buffer, vals_s.offset, vals_s.size},
        }};
        record_phase_c_dispatch_locked(command_buffer_, key_pipe, key_infos,
                                       &key_push, sizeof(key_push), ceil_div_u32(n, kSeqSortLocal));
        record_compute_barrier(command_buffer_);
        const GpuSortPlan sort_plan{n, 8u, 8u, 8u, 8u};   // full 64-bit key, 8 LSD passes
        record_radix_sort_locked(command_buffer_, keys_s, vals_s, keys_scr_s, vals_scr_s, hist_s,
                                 sort_plan, swg.num_workgroups, swg.elems_per_wg, 1u, sorted_in_scratch);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(seqsort1)");
    }

    // Sorted keys/vals live in keys/vals or their scratch, per the ping-pong parity.
    const std::uint64_t* sorted_keys =
        static_cast<const std::uint64_t*>((sorted_in_scratch ? keys_scr : keys).mapped);
    const std::uint64_t* sorted_vals =
        static_cast<const std::uint64_t*>((sorted_in_scratch ? vals_scr : vals).mapped);

    // Seed `index` from the sorted row ids (ascending by key, radix-stable on ties).
    for (std::uint32_t i = 0; i < n; ++i)
        index_ptr[i] = static_cast<std::uint32_t>(sorted_vals[i] & 0xffffffffu);
    for (std::uint32_t i = n; i < padded_n; ++i)
        index_ptr[i] = 0xffffffffu;

    // Build equal-key run ranges (only runs of length > 1 need a tie-break).
    struct RunRange { std::uint32_t begin; std::uint32_t len; };
    std::vector<RunRange> ranges;
    bool has_oversized_run = false;
    for (std::uint32_t i = 0; i < n; ) {
        std::uint32_t j = i + 1u;
        while (j < n && sorted_keys[j] == sorted_keys[i]) ++j;
        if (j - i > 1u) {
            const std::uint32_t len = j - i;
            ranges.push_back(RunRange{i, len});
            has_oversized_run = has_oversized_run || len > kSeqSortLocal;
        }
        i = j;
    }

    if (ranges.empty()) {
        // No equal-key collisions: the 64-bit key fully ordered the batch.
        std::vector<std::uint32_t> perm(index_ptr, index_ptr + n);
        bump();
        return perm;
    }

    // --- Submit 2: GPU full-compare tie-break. Oversized collisions continue
    // through a device-side global bitonic network before the one host readback.
    Buffer ranges_buf = create_device_buffer(std::uint64_t{ranges.size()} * 8u, kPhaseBBufferUsage);
    if (ranges_buf.mapped == nullptr)
        throw VulkanRuntimeError("run_sequence_sort_roundtrip: ranges buffer alloc failed");
    std::memcpy(ranges_buf.mapped, ranges.data(), ranges.size() * sizeof(RunRange));
    const GpuArenaSlice ranges_s{&ranges_buf, 0, ranges_buf.bytes};
    const SeqSortPush tb_push{
        static_cast<std::uint32_t>(ranges.size()), params.unit_bits, params.bits_per_char,
        params.letters_per_unit, params.char_mask,
        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
        static_cast<std::uint32_t>(params.used_storage_mask >> 32), 0u, 0u, 0u};
    std::uint64_t global_compare_dispatches = 0;
    {
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort2)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort2)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> tb_infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer, descs_s.offset, descs_s.size},
            {index_s.buffer->buffer, index_s.offset, index_s.size},
            {ranges_s.buffer->buffer, ranges_s.offset, ranges_s.size},
        }};
        record_phase_c_dispatch_locked(command_buffer_, tb_pipe, tb_infos,
                                       &tb_push, sizeof(tb_push),
                                       static_cast<std::uint32_t>(ranges.size()));
        if (has_oversized_run) {
            const std::array<VkDescriptorBufferInfo, 4> gc_infos{{
                {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
                {descs_s.buffer->buffer, descs_s.offset, descs_s.size},
                {index_s.buffer->buffer, index_s.offset, index_s.size},
                {ranges_s.buffer->buffer, ranges_s.offset, ranges_s.size},
            }};
            for (std::uint32_t sort_size = 2u; sort_size <= padded_n; ) {
                for (std::uint32_t stride = sort_size >> 1u; stride > 0u; stride >>= 1u) {
                    record_compute_barrier(command_buffer_);
                    SeqSortPush gc_push{
                        padded_n, params.unit_bits, params.bits_per_char,
                        params.letters_per_unit, params.char_mask,
                        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
                        static_cast<std::uint32_t>(params.used_storage_mask >> 32),
                        0u, sort_size, stride};
                    record_phase_c_dispatch_locked(
                        command_buffer_, gc_pipe, gc_infos, &gc_push, sizeof(gc_push),
                        ceil_div_u32(padded_n, kSeqSortLocal));
                    ++global_compare_dispatches;
                }
                if (sort_size == padded_n) break;
                sort_size <<= 1u;
            }
        }
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(seqsort2)");
    }

    if (global_compare_dispatches != 0) {
        seq_sort_global_compare_dispatch_count_.fetch_add(
            global_compare_dispatches, std::memory_order_relaxed);
    }
    index_ptr = static_cast<std::uint32_t*>(index_buf.mapped);  // mapping is stable; re-fetch defensively
    std::vector<std::uint32_t> perm(index_ptr, index_ptr + n);
    bump();
    return perm;
}

std::vector<GpuAlignmentExtensionResultWire> Runtime::run_alignment_extension_roundtrip(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const GpuAlignmentExtensionJobWire> jobs,
    const GpuAlignmentExtensionParams& params)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_alignment_extension_roundtrip on unavailable device");
    }
    if (jobs.empty()) {
        return {};
    }
    validate_alignment_extension_inputs(query_symbols, target_symbols, jobs, params);
    if (features_.max_workgroup_count_x == 0) {
        throw VulkanRuntimeError("alignment extension device reports zero max workgroup count");
    }

    constexpr std::uint32_t kAlignmentExtensionLocal = 1u;
    const PhaseBPipeline pipe = get_phase_b_pipeline_locked(
        ShaderKind::align_extend_banded_affine,
        kAlignmentExtensionLocal,
        k_gpu_alignment_extension_max_len);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(query_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(target_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size()) * sizeof(GpuAlignmentExtensionJobWire), 4u);
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size()) * sizeof(GpuAlignmentExtensionResultWire);

    Buffer query_buf = create_device_buffer(query_bytes, kPhaseBBufferUsage);
    Buffer target_buf = create_device_buffer(target_bytes, kPhaseBBufferUsage);
    Buffer jobs_buf = create_device_buffer(job_bytes, kPhaseBBufferUsage);
    Buffer results_buf = create_device_buffer(result_bytes, kPhaseBBufferUsage);
    if (query_buf.mapped == nullptr || target_buf.mapped == nullptr
        || jobs_buf.mapped == nullptr || results_buf.mapped == nullptr) {
        throw VulkanRuntimeError("run_alignment_extension_roundtrip: device buffer alloc failed");
    }
    if (!query_symbols.empty()) {
        std::memcpy(query_buf.mapped, query_symbols.data(),
                    query_symbols.size() * sizeof(std::uint32_t));
    }
    if (!target_symbols.empty()) {
        std::memcpy(target_buf.mapped, target_symbols.data(),
                    target_symbols.size() * sizeof(std::uint32_t));
    }
    std::memcpy(jobs_buf.mapped, jobs.data(), jobs.size() * sizeof(GpuAlignmentExtensionJobWire));
    std::memset(results_buf.mapped, 0, static_cast<std::size_t>(result_bytes));

    const std::array<VkDescriptorBufferInfo, 4> infos{{
        {query_buf.buffer, 0, query_buf.bytes},
        {target_buf.buffer, 0, target_buf.bytes},
        {jobs_buf.buffer, 0, jobs_buf.bytes},
        {results_buf.buffer, 0, results_buf.bytes},
    }};
    const AlignmentExtensionPush push = alignment_extension_push(
        static_cast<std::uint32_t>(jobs.size()), params);
    const std::uint32_t groups = std::min(
        static_cast<std::uint32_t>(jobs.size()), features_.max_workgroup_count_x);

    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_ext)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(align_ext)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, pipe, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(align_ext)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_ext_post)");

    std::vector<GpuAlignmentExtensionResultWire> results(jobs.size());
    std::memcpy(results.data(), results_buf.mapped, static_cast<std::size_t>(result_bytes));
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].status != k_gpu_alignment_extension_status_ok) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension shader rejected job {} with status {}",
                i, results[i].status));
        }
    }
    alignment_extension_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return results;
}

std::vector<GpuSneakySnakeResultWire>
Runtime::run_sneaky_snake_rank_batch_roundtrip(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const GpuSneakySnakeJobWire> jobs)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError(
            "run_sneaky_snake_rank_batch_roundtrip on unavailable device");
    }
    if (jobs.empty()) {
        return {};
    }
    validate_sneaky_snake_inputs(query_ranks, target_ranks, jobs);
    if (features_.max_workgroup_count_x == 0u) {
        throw VulkanRuntimeError(
            "SneakySnake device reports zero max workgroup count");
    }

    constexpr std::uint32_t kSneakySnakeLocal = 64u;
    const PhaseBPipeline pipe = get_phase_b_pipeline_locked(
        ShaderKind::sneaky_snake_rank_batch,
        kSneakySnakeLocal,
        k_gpu_sneaky_snake_submaze_length);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(query_ranks.size())
            * sizeof(std::uint32_t),
        sizeof(std::uint32_t));
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(target_ranks.size())
            * sizeof(std::uint32_t),
        sizeof(std::uint32_t));
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size())
            * sizeof(GpuSneakySnakeJobWire),
        sizeof(std::uint32_t));
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size())
        * sizeof(GpuSneakySnakeResultWire);

    for (const auto [label, bytes] : {
            std::pair{"query ranks", query_bytes},
            std::pair{"target ranks", target_bytes},
            std::pair{"jobs", job_bytes},
            std::pair{"results", result_bytes}}) {
        if (bytes > features_.max_storage_buffer_range) {
            throw VulkanRuntimeError(fmt::format(
                "SneakySnake {} buffer {} exceeds maxStorageBufferRange {}",
                label, bytes, features_.max_storage_buffer_range));
        }
    }

    Buffer query_buf = create_device_buffer(query_bytes, kPhaseBBufferUsage);
    Buffer target_buf = create_device_buffer(target_bytes, kPhaseBBufferUsage);
    Buffer jobs_buf = create_device_buffer(job_bytes, kPhaseBBufferUsage);
    Buffer results_buf = create_device_buffer(result_bytes, kPhaseBBufferUsage);
    if (query_buf.mapped == nullptr || target_buf.mapped == nullptr
        || jobs_buf.mapped == nullptr || results_buf.mapped == nullptr) {
        throw VulkanRuntimeError(
            "run_sneaky_snake_rank_batch_roundtrip: device buffer alloc failed");
    }

    auto* query_words = static_cast<std::uint32_t*>(query_buf.mapped);
    for (std::size_t i = 0; i < query_ranks.size(); ++i) {
        query_words[i] = query_ranks[i];
    }
    if (query_ranks.empty()) query_words[0] = 0u;
    auto* target_words = static_cast<std::uint32_t*>(target_buf.mapped);
    for (std::size_t i = 0; i < target_ranks.size(); ++i) {
        target_words[i] = target_ranks[i];
    }
    if (target_ranks.empty()) target_words[0] = 0u;
    std::memcpy(
        jobs_buf.mapped,
        jobs.data(),
        jobs.size() * sizeof(GpuSneakySnakeJobWire));
    std::memset(
        results_buf.mapped,
        0,
        static_cast<std::size_t>(result_bytes));

    const std::array<VkDescriptorBufferInfo, 4> infos{{
        {query_buf.buffer, 0, query_buf.bytes},
        {target_buf.buffer, 0, target_buf.bytes},
        {jobs_buf.buffer, 0, jobs_buf.bytes},
        {results_buf.buffer, 0, results_buf.bytes},
    }};
    const GpuSneakySnakePush push{
        static_cast<std::uint32_t>(jobs.size())};
    const std::uint64_t groups_needed =
        (static_cast<std::uint64_t>(jobs.size()) + kSneakySnakeLocal - 1u)
        / kSneakySnakeLocal;
    const std::uint32_t groups = static_cast<std::uint32_t>(
        std::min(
            groups_needed,
            static_cast<std::uint64_t>(features_.max_workgroup_count_x)));
    if (groups == 0u) {
        throw VulkanRuntimeError(
            "SneakySnake grid-stride dispatch has zero workgroups");
    }

    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(sneaky_snake)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(
        vkBeginCommandBuffer(command_buffer_, &begin),
        "begin(sneaky_snake)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, pipe, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    submit_and_wait(
        device_, queue_, command_buffer_, fence_, "submit(sneaky_snake)");
    check_vk(
        vkResetCommandBuffer(command_buffer_, 0),
        "reset(sneaky_snake_post)");

    std::vector<GpuSneakySnakeResultWire> results(jobs.size());
    std::memcpy(
        results.data(),
        results_buf.mapped,
        static_cast<std::size_t>(result_bytes));
    validate_sneaky_snake_results(jobs, results);
    sneaky_snake_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return results;
}

AlignmentCigarBatchResult Runtime::run_alignment_simple_cigar_roundtrip(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const GpuAlignmentCigarJobWire> jobs,
    const GpuAlignmentCigarParams& params,
    std::uint32_t packed_cigar_capacity)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_alignment_simple_cigar_roundtrip on unavailable device");
    }
    if (jobs.empty()) {
        return {};
    }
    validate_alignment_cigar_inputs(
        query_symbols, target_symbols, jobs, params, packed_cigar_capacity);
    if (features_.max_workgroup_count_x == 0) {
        throw VulkanRuntimeError(
            "alignment CIGAR extension device reports zero max workgroup count");
    }

    constexpr std::uint32_t kAlignmentCigarLocal = 64u;
    const PhaseBPipeline pipe = get_phase_b_pipeline_locked(
        ShaderKind::align_extend_simple_cigar,
        kAlignmentCigarLocal,
        k_gpu_alignment_extension_max_len);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(query_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(target_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size()) * sizeof(GpuAlignmentCigarJobWire), 4u);
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size()) * sizeof(GpuAlignmentCigarResultWire);
    const std::uint64_t cigar_bytes =
        static_cast<std::uint64_t>(packed_cigar_capacity) * sizeof(std::uint32_t);
    std::uint64_t traceback_words = 0;
    for (const GpuAlignmentCigarJobWire& job : jobs) {
        const std::uint64_t end =
            static_cast<std::uint64_t>(job.traceback_tile_offset)
            + static_cast<std::uint64_t>(job.traceback_tile_count);
        traceback_words = std::max(traceback_words, end);
    }
    const std::uint64_t traceback_bytes =
        std::max<std::uint64_t>(traceback_words * sizeof(std::uint32_t), 4u);

    Buffer query_buf = create_device_buffer(query_bytes, kPhaseBBufferUsage);
    Buffer target_buf = create_device_buffer(target_bytes, kPhaseBBufferUsage);
    Buffer jobs_buf = create_device_buffer(job_bytes, kPhaseBBufferUsage);
    Buffer results_buf = create_device_buffer(result_bytes, kPhaseBBufferUsage);
    Buffer cigar_buf = create_device_buffer(cigar_bytes, kPhaseBBufferUsage);
    Buffer traceback_buf = create_device_buffer(traceback_bytes, kPhaseBBufferUsage);
    if (query_buf.mapped == nullptr || target_buf.mapped == nullptr
        || jobs_buf.mapped == nullptr || results_buf.mapped == nullptr
        || cigar_buf.mapped == nullptr || traceback_buf.mapped == nullptr) {
        throw VulkanRuntimeError(
            "run_alignment_simple_cigar_roundtrip: device buffer alloc failed");
    }
    if (!query_symbols.empty()) {
        std::memcpy(query_buf.mapped, query_symbols.data(),
                    query_symbols.size() * sizeof(std::uint32_t));
    }
    if (!target_symbols.empty()) {
        std::memcpy(target_buf.mapped, target_symbols.data(),
                    target_symbols.size() * sizeof(std::uint32_t));
    }
    std::memcpy(jobs_buf.mapped, jobs.data(),
                jobs.size() * sizeof(GpuAlignmentCigarJobWire));
    std::memset(results_buf.mapped, 0, static_cast<std::size_t>(result_bytes));
    std::memset(cigar_buf.mapped, 0, static_cast<std::size_t>(cigar_bytes));

    const std::array<VkDescriptorBufferInfo, 6> infos{{
        {query_buf.buffer, 0, query_buf.bytes},
        {target_buf.buffer, 0, target_buf.bytes},
        {jobs_buf.buffer, 0, jobs_buf.bytes},
        {results_buf.buffer, 0, results_buf.bytes},
        {cigar_buf.buffer, 0, cigar_buf.bytes},
        {traceback_buf.buffer, 0, traceback_buf.bytes},
    }};
    const AlignmentCigarPush push = alignment_cigar_push(
        static_cast<std::uint32_t>(jobs.size()), params);
    const auto job_count_u64 = static_cast<std::uint64_t>(jobs.size());
    const std::uint64_t groups_u64 =
        (job_count_u64 + kAlignmentCigarLocal - 1u) / kAlignmentCigarLocal;
    const std::uint32_t groups = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(groups_u64, features_.max_workgroup_count_x));

    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_cigar)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(align_cigar)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, pipe, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(align_cigar)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_cigar_post)");

    AlignmentCigarBatchResult out{};
    out.results.resize(jobs.size());
    out.packed_cigar.resize(packed_cigar_capacity);
    std::memcpy(out.results.data(), results_buf.mapped,
                static_cast<std::size_t>(result_bytes));
    std::memcpy(out.packed_cigar.data(), cigar_buf.mapped,
                static_cast<std::size_t>(cigar_bytes));
    for (std::size_t i = 0; i < out.results.size(); ++i) {
        if (out.results[i].status != k_gpu_alignment_extension_status_ok) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension shader rejected job {} with status {}",
                i, out.results[i].status));
        }
    }
    alignment_extension_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

namespace {

constexpr std::uint32_t kPhaseCSeedCoordinateBatchWave = 4096u;

struct ChainRangePushLocal {
    std::uint32_t query_count;
    std::uint32_t max_ref_gap;
};
static_assert(sizeof(ChainRangePushLocal) == 8);

struct PhaseCCoordinateVoteJobWire {
    std::uint32_t anchor_offset;
    std::uint32_t anchor_count;
    std::uint32_t query_len_lo;
    std::uint32_t query_len_hi;
};
static_assert(sizeof(PhaseCCoordinateVoteJobWire) == 16);

struct PhaseCSeedCoordinateWaveState {
    std::size_t result_index = 0;
    PhaseCSeedCoordinateJob job{};
    PhaseCSeedQueryRecordWire query_record{};
    PersistedShapeV5CoordinateJobAudit operation_audit{};
    std::uint32_t minimizer_count = 0;
    std::uint32_t num_blocks = 0;
    std::uint32_t seed_groups = 0;
    std::uint64_t query_len = 0;
    GpuArenaSlice counts_s{};
    GpuArenaSlice prefix_s{};
    GpuArenaSlice blocks_s{};
    SeedPushC seed_push{};
    GpuShapeV5SeedPush shape_v5_seed_push{};
    std::uint64_t total_anchors = 0;
    std::uint32_t total_anchors_u32 = 0;
    std::uint32_t anchor_offset = 0;
    std::uint32_t vote_result_index = 0;
    WgPlan anchor_wg{};
    Buffer anchors_buf;
    Buffer anchor_keys_buf;
    Buffer anchors_scratch_buf;
    Buffer anchor_keys_scratch_buf;
    Buffer anchor_histogram_buf;
    Buffer ranges_buf;
    Buffer range_count_buf;
    Buffer compacted_anchors_buf;
    Buffer span_buf;
    bool sorted_in_scratch = false;
    std::uint32_t range_count = 0;
    std::uint32_t compacted_anchor_count = 0;
    std::uint32_t active_anchor_count = 0;
    Buffer score_buf;
    Buffer predecessor_buf;
    Buffer visited_buf;
    Buffer trace_buf;
    Buffer result_buf;
    bool skip_seed = false;
    bool has_anchors = false;
    bool has_ranges = false;
	bool chain_active = false;

    [[nodiscard]] GpuArenaSlice anchors_slice() noexcept {
        return GpuArenaSlice{&anchors_buf, 0, anchors_buf.bytes};
    }
    [[nodiscard]] GpuArenaSlice anchor_keys_slice() noexcept {
        return GpuArenaSlice{&anchor_keys_buf, 0, anchor_keys_buf.bytes};
    }
    [[nodiscard]] GpuArenaSlice anchors_scratch_slice() noexcept {
        return GpuArenaSlice{&anchors_scratch_buf, 0, anchors_scratch_buf.bytes};
    }
    [[nodiscard]] GpuArenaSlice anchor_keys_scratch_slice() noexcept {
        return GpuArenaSlice{&anchor_keys_scratch_buf, 0, anchor_keys_scratch_buf.bytes};
    }
    [[nodiscard]] GpuArenaSlice anchor_histogram_slice() noexcept {
        return GpuArenaSlice{&anchor_histogram_buf, 0, anchor_histogram_buf.bytes};
    }
    [[nodiscard]] GpuArenaSlice compacted_anchors_slice() noexcept {
        return GpuArenaSlice{&compacted_anchors_buf, 0, compacted_anchors_buf.bytes};
    }
};

[[nodiscard]] bool phase_c_same_coordinate_target(
    const PhaseCSeedCoordinateJob& lhs,
    const PhaseCSeedCoordinateJob& rhs) noexcept
{
    return lhs.shape_v5 == rhs.shape_v5
        && lhs.slots == rhs.slots
        && lhs.positions == rhs.positions
        && lhs.target_len == rhs.target_len
        && lhs.slot_mask == rhs.slot_mask
        && lhs.position_count == rhs.position_count
        && lhs.bucket_count == rhs.bucket_count
        && lhs.target_position_offset == rhs.target_position_offset;
}

}  // namespace

PhaseCSeedCoordinateBatchRun
Runtime::run_phase_c_seed_to_coordinate_multi_query_batch_roundtrip(
    const PhaseCSeedQueryInputs& query_inputs,
    std::span<const PhaseCSeedCoordinateJob> jobs,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    PhaseCSeedCoordinateAlgorithm coordinate_algorithm,
    const ChainingPipelineKey& chain_key,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit,
    DeviceQuerySketchAuthority query_authority)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError(
            "run_phase_c_seed_to_coordinate_batch_roundtrip on unavailable device");
    }
    if (features_.max_workgroup_count_x == 0u) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate batch has zero workgroup limit");
    }
    if (coordinate_algorithm != PhaseCSeedCoordinateAlgorithm::vulkan_tiled_dp
        && coordinate_algorithm
            != PhaseCSeedCoordinateAlgorithm::coordinate_vote) {
        throw VulkanRuntimeError(
            "phase_c seed-to-coordinate algorithm identity is invalid");
    }
    const bool coordinate_vote_selected =
        coordinate_algorithm
        == PhaseCSeedCoordinateAlgorithm::coordinate_vote;
    if (query_inputs.device_query_records != nullptr) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate device query records are not implemented");
    }

    const PhaseCSeedIndexRepresentation representation = jobs.empty()
        ? PhaseCSeedIndexRepresentation::invalid
        : phase_c_seed_index_representation(jobs.front());
    const bool shape_v5_selected =
        representation == PhaseCSeedIndexRepresentation::shape_v5;
    if (shape_v5_selected && coordinate_vote_selected) {
        throw VulkanRuntimeError(
            "phase_c Shape Format-5 direct route requires Vulkan tiled-DP coordinate output");
    }
    if (shape_v5_selected && features_.software_device) {
        throw VulkanRuntimeError(
            "phase_c Shape Format-5 direct route rejects software Vulkan devices");
    }
    const auto shape_operation_configuration =
        persisted_shape_v5_coordinate_config_identity(
            kmer_size,
            max_occ,
            coordinate_algorithm,
            chain_key,
            max_dist_x,
            max_dist_y,
            bandwidth,
            max_iter,
            gap_penalty,
            skip_penalty,
            max_chain_skip,
            min_chain_score,
            min_seed_count,
            probe_limit);
    if (shape_v5_selected && !shape_operation_configuration.accepted()) {
        throw VulkanRuntimeError(
            "phase_c Shape Format-5 coordinate configuration identity is invalid");
    }

    const PhaseCShapeV5ResidentView* common_shape_view = nullptr;
    std::uint64_t common_shape_target_len = 0u;
    for (const PhaseCSeedCoordinateJob& job : jobs) {
        const PhaseCSeedIndexRepresentation job_representation =
            phase_c_seed_index_representation(job);
        if (job_representation == PhaseCSeedIndexRepresentation::invalid
            || job_representation != representation) {
            throw VulkanRuntimeError(
                "phase_c seed-to-coordinate resident index representation must be strict XOR and uniform");
        }
        if (job.query_index >= query_inputs.host_query_records.size()) {
            throw VulkanRuntimeError("phase_c seed-to-coordinate job query_index out of range");
        }
        if (job.target_len == 0u) {
            throw VulkanRuntimeError(
                "phase_c seed-to-coordinate target length must be nonzero");
        }
        if (shape_v5_selected) {
            if (job.target_len > (std::uint64_t{1} << 63u)) {
                throw VulkanRuntimeError(
                    "phase_c Shape Format-5 target length exceeds anchor-key ABI");
            }
            if (common_shape_view == nullptr) {
                common_shape_view = job.shape_v5;
                common_shape_target_len = job.target_len;
            } else if (job.shape_v5 != common_shape_view) {
                throw VulkanRuntimeError(
                    "phase_c Shape Format-5 batch must bind one resident generation");
            } else if (job.target_len != common_shape_target_len) {
                throw VulkanRuntimeError(
                    "phase_c Shape Format-5 batch target identity is inconsistent");
            }
        }
    }

    PhaseCSeedCoordinateBatchRun run{};
    run.results.resize(jobs.size());
    run.stats.problem_count = static_cast<std::uint64_t>(jobs.size());
    run.shape_v5_direct.selected = shape_v5_selected;
    run.shape_v5_direct.problem_count = static_cast<std::uint64_t>(jobs.size());

    std::uint32_t shape_control_word_count = 0u;
    std::uint32_t shape_position_word_count = 0u;
    std::uint64_t shape_control_padded_bytes = 0u;
    std::uint64_t shape_position_padded_bytes = 0u;
    if (shape_v5_selected) {
        if (common_shape_view == nullptr) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 route has no resident generation");
        }
        if (kmer_size == 0u || kmer_size > 255u
            || query_inputs.hash_shift >= 64u
            || query_inputs.pos_bits >= 64u) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 query sketch traits exceed ABI3");
        }
        const auto valid_resident_buffer = [this](const Buffer* buffer) {
            return buffer != nullptr
                && buffer->owner == this
                && buffer->valid()
                && buffer->host_visible_coherent()
                && buffer->mapped != nullptr;
        };
        if (!valid_resident_buffer(common_shape_view->descriptors)
            || !valid_resident_buffer(common_shape_view->controls)
            || !valid_resident_buffer(common_shape_view->positions)) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 resident buffers are invalid, foreign, or not coherent");
        }
        if (common_shape_view->descriptors->buffer
                == common_shape_view->controls->buffer
            || common_shape_view->descriptors->buffer
                == common_shape_view->positions->buffer
            || common_shape_view->controls->buffer
                == common_shape_view->positions->buffer) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 resident buffers must not alias");
        }
        if (common_shape_view->abi_version != k_gpu_shape_v5_abi_version
            || common_shape_view->partition_count == 0u
            || common_shape_view->control_bytes == 0u
            || common_shape_view->position_bytes == 0u
            || common_shape_view->position_count == 0u
            || common_shape_view->max_occ_cap
                == (std::numeric_limits<std::uint64_t>::max)()
            || common_shape_view->generation_id == 0u
            || common_shape_view->source_identity == 0u
            || common_shape_view->cold_load_identity == 0u) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 resident identity or generation contract is invalid");
        }
        const std::uint64_t expected_descriptor_bytes =
            static_cast<std::uint64_t>(common_shape_view->partition_count)
            * sizeof(GpuShapeV5ResidentPartitionDescriptor);
        if (common_shape_view->descriptor_bytes != expected_descriptor_bytes
            || common_shape_view->descriptor_bytes
                > common_shape_view->descriptors->bytes) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 descriptor byte extent is invalid");
        }
        const auto checked_word_count = [](std::uint64_t bytes,
                                           const char* label) {
            constexpr std::uint64_t maximum_word_bytes =
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)()) * 4u;
            if (bytes == 0u || bytes > maximum_word_bytes) {
                throw VulkanRuntimeError(fmt::format(
                    "phase_c Shape Format-5 {} byte extent exceeds GLSL word ABI",
                    label));
            }
            return static_cast<std::uint32_t>((bytes + 3u) / 4u);
        };
        shape_control_word_count = checked_word_count(
            common_shape_view->control_bytes, "control");
        shape_position_word_count = checked_word_count(
            common_shape_view->position_bytes, "position");
        shape_control_padded_bytes =
            static_cast<std::uint64_t>(shape_control_word_count) * 4u;
        shape_position_padded_bytes =
            static_cast<std::uint64_t>(shape_position_word_count) * 4u;
        if (shape_control_padded_bytes > common_shape_view->controls->bytes
            || shape_position_padded_bytes
                > common_shape_view->positions->bytes
            || common_shape_view->descriptor_bytes
                > features_.max_storage_buffer_range
            || shape_control_padded_bytes > features_.max_storage_buffer_range
            || shape_position_padded_bytes
                > features_.max_storage_buffer_range) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 resident byte extent exceeds its buffer or Vulkan range");
        }
        const bool nil_view_uuid = std::all_of(
            common_shape_view->device_uuid.begin(),
            common_shape_view->device_uuid.end(),
            [](std::uint8_t byte) { return byte == 0u; });
        if (nil_view_uuid
            || common_shape_view->device_uuid != features_.device_uuid) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 resident generation belongs to a different Vulkan device");
        }
        const auto* descriptor_bytes = static_cast<const std::byte*>(
            common_shape_view->descriptors->mapped);
        for (std::uint32_t ordinal = 0u;
             ordinal < common_shape_view->partition_count;
             ++ordinal) {
            GpuShapeV5ResidentPartitionDescriptor descriptor{};
            std::memcpy(
                &descriptor,
                descriptor_bytes
                    + static_cast<std::uint64_t>(ordinal)
                        * sizeof(GpuShapeV5ResidentPartitionDescriptor),
                sizeof(descriptor));
            if (!gpu_shape_v5_partition_descriptor_valid(
                    descriptor,
                    common_shape_view->control_bytes,
                    common_shape_view->position_bytes)
                || descriptor.segment_end_bp > common_shape_target_len) {
                throw VulkanRuntimeError(
                    "phase_c Shape Format-5 resident descriptor validation failed");
            }
        }
        if (max_occ >= 0
            && static_cast<std::uint64_t>(max_occ)
                > common_shape_view->max_occ_cap) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 query max_occ exceeds persisted generation cap");
        }
        run.shape_v5_direct.generation_id = common_shape_view->generation_id;
        run.shape_v5_direct.source_identity = common_shape_view->source_identity;
        run.shape_v5_direct.cold_load_identity =
            common_shape_view->cold_load_identity;
    }
    const std::uint32_t total_mc = query_inputs.minimizer_count;
    if (jobs.empty() || max_occ < 0
        || (total_mc == 0u && !shape_v5_selected)) {
        run.shape_v5_direct.direct_count_scan_emit_completed =
            shape_v5_selected;
        return run;
    }
    if (query_inputs.host_query_records.empty()) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate batch missing query records");
    }
    for (const PhaseCSeedQueryRecordWire& qrec : query_inputs.host_query_records) {
        const std::uint64_t end =
            static_cast<std::uint64_t>(qrec.minimizer_offset)
            + qrec.minimizer_count;
        if (qrec.flags != 0u || qrec.minimizer_offset > total_mc
            || end > total_mc) {
            throw VulkanRuntimeError(
                "phase_c seed-to-coordinate query minimizer slice or flags are invalid");
        }
        if (qrec.minimizer_count == 0u) {
            continue;
        }
        if (shape_v5_selected
            && (qrec.query_len < kmer_size
                || qrec.minimizer_count
                    == (std::numeric_limits<std::uint32_t>::max)()
                || (static_cast<std::uint64_t>(qrec.minimizer_count) + 1u)
                        * sizeof(std::uint32_t)
                    > features_.max_storage_buffer_range)) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 query record violates local count/status ABI");
        }
    }

    const std::uint32_t required_words_per_minimizer =
        query_inputs.minimizer_word_kind == 0u
        || query_inputs.minimizer_word_kind == 1u
        ? 1u
        : query_inputs.minimizer_word_kind == 2u ? 2u : 4u;
    if (query_inputs.minimizer_word_kind > 3u
        || query_inputs.minimizer_words32_per_record
            < required_words_per_minimizer
        || (query_inputs.minimizer_word_kind != 0u
            && static_cast<std::uint64_t>(total_mc)
                > (std::numeric_limits<std::uint64_t>::max)()
                    / (static_cast<std::uint64_t>(
                           query_inputs.minimizer_words32_per_record)
                       * sizeof(std::uint32_t)))) {
        throw VulkanRuntimeError(
            "phase_c seed-to-coordinate minimizer word shape is invalid or overflows");
    }
    const std::uint64_t required_min_words_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(
            query_inputs.minimizer_word_kind,
            query_inputs.minimizer_words32_per_record,
            total_mc),
        4u);
    if (query_inputs.device_minimizer_words != nullptr
        && (!query_inputs.device_minimizer_words->valid()
            || query_inputs.device_minimizer_words->owner != this
            || required_min_words_bytes
                > query_inputs.device_minimizer_words->bytes)) {
        throw VulkanRuntimeError(
            "phase_c seed-to-coordinate device minimizer buffer is invalid or undersized");
    }
    if (shape_v5_selected
        && (query_inputs.device_minimizer_words == nullptr
            || !query_inputs.host_minimizer_bytes.empty())) {
        throw VulkanRuntimeError(
            "phase_c Shape Format-5 direct route requires an unambiguous GPU-resident query sketch");
    }
    std::unique_ptr<RuntimePipelineUseCollector> shape_pipeline_collector;
    if (shape_v5_selected) {
        bool authority_records_match =
            query_authority.query_records_.size()
            == query_inputs.host_query_records.size();
        for (std::size_t record_index = 0u;
             authority_records_match
                 && record_index < query_authority.query_records_.size();
             ++record_index) {
            const auto& sealed = query_authority.query_records_[record_index];
            const auto& supplied =
                query_inputs.host_query_records[record_index];
            authority_records_match = sealed.minimizer_offset
                    == supplied.minimizer_offset
                && sealed.minimizer_count == supplied.minimizer_count
                && sealed.query_len == supplied.query_len
                && sealed.flags == supplied.flags;
        }
        if (!query_authority.accepted()
            || query_authority.runtime_ != this
            || query_authority.buffer_
                != query_inputs.device_minimizer_words->buffer
            || query_authority.memory_identity_
                != vulkan_handle_identity(
                    query_inputs.device_minimizer_words->memory)
            || query_authority.buffer_bytes_
                != query_inputs.device_minimizer_words->bytes
            || query_authority.allocation_bytes_
                != query_inputs.device_minimizer_words->allocation_bytes
            || query_authority.minimizer_count_ != total_mc
            || query_authority.kmer_size_ != kmer_size
            || query_authority.window_size_ != query_inputs.window_size
            || query_authority.minimizer_word_kind_
                != query_inputs.minimizer_word_kind
            || query_authority.minimizer_words32_per_record_
                != query_inputs.minimizer_words32_per_record
            || query_authority.hash_shift_ != query_inputs.hash_shift
            || query_authority.pos_bits_ != query_inputs.pos_bits
            || !authority_records_match
            || query_authority.device_uuid_ != features_.device_uuid) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 direct route lacks matching one-shot query authority");
        }
        if (query_authority.buffer_
                == common_shape_view->descriptors->buffer
            || query_authority.buffer_
                == common_shape_view->controls->buffer
            || query_authority.buffer_
                == common_shape_view->positions->buffer) {
            throw VulkanRuntimeError(
                "phase_c Shape Format-5 query image aliases resident target storage");
        }
        run.operation_proof.operation_identity_ =
            next_operation_proof_identity_locked();
        run.operation_proof.admit_records(jobs.size());
        shape_pipeline_collector = RuntimePipelineUseCollector::create(
            RuntimePipelineOperationKind::shape_v5_coordinate,
            jobs.size(),
            run.operation_proof.operation_identity_);
    } else if (query_authority.accepted()) {
        throw VulkanRuntimeError(
            "phase_c query authority cannot be consumed by a legacy index route");
    }
    const std::uint64_t min_words_bytes =
        query_inputs.device_minimizer_words == nullptr
        ? required_min_words_bytes : 0u;
    if (query_inputs.device_minimizer_words == nullptr
        && query_inputs.host_minimizer_bytes.size()
            > static_cast<std::size_t>(min_words_bytes)) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate minimizer upload exceeds buffer");
    }

    const PhaseBPipeline count_pipe = get_phase_b_pipeline_locked(
        shape_v5_selected
            ? ShaderKind::phase_c_shape_v5_seed_count
            : ShaderKind::phase_c_seed_count,
        128u,
        shape_v5_selected ? 3u : 1u);
    const PhaseBPipeline emit_pipe = get_phase_b_pipeline_locked(
        shape_v5_selected
            ? ShaderKind::phase_c_shape_v5_seed_emit
            : ShaderKind::phase_c_seed_emit,
        128u,
        shape_v5_selected ? 3u : 1u);
    const PhaseBPipeline scanb =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scana =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);
    const PhaseBPipeline range_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_chain_range_build, 1u, 1u);
    const PhaseBPipeline coord_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_coordinate_reduce, 128u, 1u);
    PhaseBPipeline vote_coord_pipe{};
    PhaseBPipeline vote_coord_batch_pipe{};
    if (coordinate_vote_selected) {
        vote_coord_pipe = get_phase_b_pipeline_locked(
            ShaderKind::phase_c_coordinate_vote_reduce, 128u, 1u);
        vote_coord_batch_pipe = get_phase_b_pipeline_locked(
            ShaderKind::phase_c_coordinate_vote_reduce_batch, 128u, 1u);
    }
    const ChainingPipeline chain_pipeline = create_pipeline_locked(chain_key);
    if (shape_pipeline_collector != nullptr) {
        const auto prepare = [&shape_pipeline_collector](
            const PhaseBPipeline& pipe) {
            shape_pipeline_collector->prepare(
                make_shader_key(
                    pipe.kind, 384u, pipe.local_size, pipe.max_span),
                pipe.metal_archive_receipt);
        };
        prepare(count_pipe);
        prepare(emit_pipe);
        prepare(scanb);
        prepare(scana);
        prepare(range_pipe);
        prepare(coord_pipe);
        shape_pipeline_collector->prepare(
            chain_pipeline.shader_key,
            chain_pipeline.metal_archive_receipt);
    }

    constexpr std::uint64_t qrec_stride = kPhaseCSlotOffsetAlign;
    const std::uint64_t query_record_bytes = std::max<std::uint64_t>(
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(query_inputs.host_query_records.size()),
            qrec_stride),
        qrec_stride);

    GpuArena& arena = phase_c_arena();
    std::uint64_t submitted_waves = 0;
    for (std::size_t wave_begin = 0; wave_begin < jobs.size();) {
        std::size_t wave_end =
            std::min<std::size_t>(jobs.size(), wave_begin + kPhaseCSeedCoordinateBatchWave);
        if (coordinate_vote_selected && wave_begin + 1u < wave_end) {
            std::size_t same_target_end = wave_begin + 1u;
            while (same_target_end < wave_end
                   && phase_c_same_coordinate_target(
                       jobs[wave_begin], jobs[same_target_end])) {
                ++same_target_end;
            }
            if (same_target_end > wave_begin + 1u) {
                wave_end = same_target_end;
            }
        }
        const std::size_t next_wave_begin = wave_end;
        const std::size_t wave_count = wave_end - wave_begin;
        std::uint64_t per_wave_problem_bytes = 0u;
        for (std::size_t job_index = wave_begin; job_index < wave_end; ++job_index) {
            const PhaseCSeedQueryRecordWire& qrec =
                query_inputs.host_query_records[jobs[job_index].query_index];
            const std::uint32_t qblocks =
                std::max<std::uint32_t>(ceil_div_u32(qrec.minimizer_count, kPhaseCScanBlock), 1u);
            per_wave_problem_bytes = sat_add(
                per_wave_problem_bytes,
                sat_add(
                    sat_add(
                        std::uint64_t{qrec.minimizer_count} * 8u,
                        shape_v5_selected ? 4u : 0u),
                    std::uint64_t{qblocks} * 4u));
        }
        const std::uint64_t arena_bytes = sat_add(
            sat_add(min_words_bytes, query_record_bytes),
            sat_add(
                per_wave_problem_bytes,
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(3u * wave_count + 2u),
                    std::uint64_t{kPhaseCSlotOffsetAlign})));
        if (!arena.ensure_capacity(arena_bytes, 0u, 0u)) {
            throw VulkanRuntimeError("phase_c seed-to-coordinate batch arena grow failed");
        }
        const GpuArenaMark mark = arena.mark();
        try {
            auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
            const GpuArenaSlice qrec_s = arena.allocate_device(query_record_bytes, 16u);
            GpuArenaSlice minw_s{};
            if (query_inputs.device_minimizer_words == nullptr) {
                minw_s = arena.allocate_device(min_words_bytes, 16u);
            }
            if (!qrec_s.valid()
                || (query_inputs.device_minimizer_words == nullptr && !minw_s.valid())) {
                throw VulkanRuntimeError("phase_c seed-to-coordinate batch arena oversubscribed");
            }

            const VkBuffer min_words_buffer = query_inputs.device_minimizer_words != nullptr
                ? query_inputs.device_minimizer_words->buffer
                : minw_s.buffer->buffer;
            const std::uint64_t min_words_offset =
                query_inputs.device_minimizer_words != nullptr ? 0u : minw_s.offset;
            const std::uint64_t min_words_range =
                query_inputs.device_minimizer_words != nullptr ? VK_WHOLE_SIZE : minw_s.size;
            if (query_inputs.device_minimizer_words == nullptr) {
                std::memset(base + minw_s.offset, 0, static_cast<std::size_t>(minw_s.size));
                if (!query_inputs.host_minimizer_bytes.empty()) {
                    std::memcpy(
                        base + minw_s.offset,
                        query_inputs.host_minimizer_bytes.data(),
                        query_inputs.host_minimizer_bytes.size());
                }
            }
            std::memset(base + qrec_s.offset, 0, static_cast<std::size_t>(qrec_s.size));
            for (std::size_t qrec_index = 0;
                 qrec_index < query_inputs.host_query_records.size();
                 ++qrec_index) {
                std::memcpy(
                    base + qrec_s.offset
                        + static_cast<std::uint64_t>(qrec_index) * qrec_stride,
                    &query_inputs.host_query_records[qrec_index],
                    sizeof(PhaseCSeedQueryRecordWire));
            }

            std::vector<PhaseCSeedCoordinateWaveState> states;
            states.reserve(wave_count);
            std::uint32_t seed_active_count = 0;
            for (std::size_t job_index = wave_begin; job_index < wave_end; ++job_index) {
                PhaseCSeedCoordinateWaveState& state = states.emplace_back();
                state.result_index = job_index;
                state.job = jobs[job_index];
                state.query_record =
                    query_inputs.host_query_records[state.job.query_index];
                if (shape_v5_selected) {
                    auto& audit = state.operation_audit;
                    audit.operation_identity =
                        run.operation_proof.operation_identity_;
                    audit.query_authority_identity =
                        query_authority.identity_;
                    audit.resident_descriptors = runtime_buffer_identity(
                        common_shape_view->descriptors);
                    audit.resident_controls = runtime_buffer_identity(
                        common_shape_view->controls);
                    audit.resident_positions = runtime_buffer_identity(
                        common_shape_view->positions);
                    audit.query_minimizer_buffer = runtime_buffer_identity(
                        query_inputs.device_minimizer_words);
                    audit.device_uuid = common_shape_view->device_uuid;
                    audit.descriptor_bytes =
                        common_shape_view->descriptor_bytes;
                    audit.control_bytes = common_shape_view->control_bytes;
                    audit.position_bytes = common_shape_view->position_bytes;
                    audit.position_count = common_shape_view->position_count;
                    audit.max_occ_cap = common_shape_view->max_occ_cap;
                    audit.partition_count =
                        common_shape_view->partition_count;
                    audit.abi_version = common_shape_view->abi_version;
                    audit.generation_id = common_shape_view->generation_id;
                    audit.source_identity = common_shape_view->source_identity;
                    audit.cold_load_identity =
                        common_shape_view->cold_load_identity;
                    audit.target_len = state.job.target_len;
                    audit.problem_index = state.result_index;
                    audit.query_index = state.job.query_index;
                    audit.logical_target_row =
                        state.job.logical_target_row;
                    audit.logical_query_ordinal =
                        state.job.logical_query_ordinal;
                    audit.query_record = state.query_record;
                    audit.configuration = shape_operation_configuration;
                    audit.query_tile_count =
                        query_authority.query_tile_counts_[
                            state.job.query_index];
                    audit.query_count_dispatches =
                        query_authority.dispatch_proof_.count_dispatches();
                    audit.query_scan_dispatches =
                        query_authority.dispatch_proof_.prefix_scan_dispatches();
                    audit.query_emit_dispatches =
                        query_authority.dispatch_proof_.emit_dispatches();
                    audit.query_completed_roundtrips =
                        query_authority.dispatch_proof_.completed_roundtrips();
                }
                state.minimizer_count = state.query_record.minimizer_count;
                state.query_len = state.query_record.query_len;
                if (shape_pipeline_collector != nullptr) {
                    shape_pipeline_collector->set_shape_input(
                        state.result_index,
                        state.minimizer_count,
                        state.job.target_len,
                        state.job.target_position_offset);
                }
                state.num_blocks = std::max<std::uint32_t>(
                    ceil_div_u32(state.minimizer_count, kPhaseCScanBlock), 1u);
                state.seed_groups = std::max<std::uint32_t>(
                    1u,
                    shape_v5_selected
                        ? 1u
                        : std::min(
                              ceil_div_u32(state.minimizer_count, 128u),
                              features_.max_workgroup_count_x));
                state.skip_seed = state.minimizer_count == 0u
                    || (!shape_v5_selected && state.job.position_count == 0u);
                if (shape_v5_selected && state.minimizer_count == 0u) {
                    run.operation_proof.records_[state.result_index].audit =
                        state.operation_audit;
                }
                const std::uint64_t count_word_count = shape_v5_selected
                    ? static_cast<std::uint64_t>(state.minimizer_count) + 1u
                    : std::max<std::uint64_t>(state.minimizer_count, 1u);
                state.counts_s = arena.allocate_device(
                    count_word_count * 4u, 16u);
                state.prefix_s = arena.allocate_device(
                    std::max<std::uint64_t>(state.minimizer_count, 1u) * 4u, 16u);
                state.blocks_s = arena.allocate_device(
                    std::uint64_t{state.num_blocks} * 4u, 16u);
                if (!state.counts_s.valid() || !state.prefix_s.valid()
                    || !state.blocks_s.valid()) {
                    throw VulkanRuntimeError(
                        "phase_c seed-to-coordinate batch arena oversubscribed");
                }
                if (shape_v5_selected) {
                    const std::uint32_t zero_status = 0u;
                    std::memcpy(
                        base + state.counts_s.offset
                            + static_cast<std::uint64_t>(state.minimizer_count)
                                * sizeof(std::uint32_t),
                        &zero_status,
                        sizeof(zero_status));
                }
                const std::uint32_t target_offset_lo = static_cast<std::uint32_t>(
                    state.job.target_position_offset & 0xffffffffull);
                const std::uint32_t target_offset_hi =
                    static_cast<std::uint32_t>(state.job.target_position_offset >> 32);
                const PhaseCAnchorSortKeyPlan anchor_sort_plan =
                    phase_c_anchor_sort_key_plan(
                        state.job.target_len,
                        state.job.target_position_offset);
                state.seed_push = SeedPushC{
                    query_inputs.minimizer_words32_per_record,
                    query_inputs.minimizer_word_kind,
                    query_inputs.hash_shift,
                    query_inputs.pos_bits,
                    kmer_size,
                    state.job.slot_mask,
                    static_cast<std::uint32_t>(max_occ),
                    0u,
                    1u,
                    state.job.bucket_count,
                    target_offset_lo,
                    target_offset_hi,
                    anchor_sort_plan.target_ref_bits};
                if (shape_v5_selected) {
                    const PhaseCShapeV5ResidentView& view =
                        *state.job.shape_v5;
                    state.shape_v5_seed_push = GpuShapeV5SeedPush{
                        .abi_version = k_gpu_shape_v5_abi_version,
                        .words32_per_minimizer =
                            query_inputs.minimizer_words32_per_record,
                        .word_kind = query_inputs.minimizer_word_kind,
                        .hash_shift = query_inputs.hash_shift,
                        .pos_bits = query_inputs.pos_bits,
                        .kmer_size = kmer_size,
                        .partition_count = view.partition_count,
                        .max_occ = static_cast<std::uint32_t>(max_occ),
                        .emit_mode = 0u,
                        .record_count = 1u,
                        .target_ref_bits = anchor_sort_plan.target_ref_bits,
                        .query_minimizer_count = total_mc,
                        .control_word_count = shape_control_word_count,
                        .position_word_count = shape_position_word_count,
                        .anchor_capacity = 0u,
                        .max_occ_cap_lo = static_cast<std::uint32_t>(
                            view.max_occ_cap & 0xffffffffull),
                        .max_occ_cap_hi = static_cast<std::uint32_t>(
                            view.max_occ_cap >> 32u),
                        .status_index = state.minimizer_count,
                    };
                }
                if (!state.skip_seed) {
                    seed_active_count += 1u;
                }
            }

            if (seed_active_count == 0u) {
                arena.rewind(mark);
                wave_begin = next_wave_begin;
                continue;
            }

            if (coordinate_vote_selected) {
                std::vector<PhaseCSeedCoordinateWaveState*> active_states;
                active_states.reserve(seed_active_count);
                for (PhaseCSeedCoordinateWaveState& state : states) {
                    if (!state.skip_seed) {
                        active_states.push_back(&state);
                    }
                }

                const PhaseCSeedCoordinateWaveState* first_active =
                    active_states.empty() ? nullptr : active_states.front();
                bool same_target_wave = active_states.size() >= 2u;
                if (same_target_wave) {
                    for (const PhaseCSeedCoordinateWaveState* state : active_states) {
                        if (state->job.slots != first_active->job.slots
                            || state->job.positions != first_active->job.positions
                            || state->job.target_len != first_active->job.target_len
                            || state->job.slot_mask != first_active->job.slot_mask
                            || state->job.position_count != first_active->job.position_count
                            || state->job.bucket_count != first_active->job.bucket_count
                            || state->job.target_position_offset
                                != first_active->job.target_position_offset) {
                            same_target_wave = false;
                            break;
                        }
                    }
                }

                if (same_target_wave) {
                    const std::uint32_t total_blocks =
                        std::max<std::uint32_t>(
                            ceil_div_u32(total_mc, kPhaseCScanBlock),
                            1u);
                    const std::uint64_t counts_bytes =
                        std::max<std::uint64_t>(std::uint64_t{total_mc} * 4u, 4u);
                    const std::uint64_t blocks_bytes =
                        std::max<std::uint64_t>(std::uint64_t{total_blocks} * 4u, 4u);
                    const std::uint64_t dense_qrec_bytes =
                        std::max<std::uint64_t>(
                            static_cast<std::uint64_t>(active_states.size())
                                * sizeof(PhaseCSeedQueryRecordWire),
                            sizeof(PhaseCSeedQueryRecordWire));

                    Buffer batch_counts_buf =
                        create_device_buffer(counts_bytes, kPhaseBBufferUsage);
                    Buffer batch_prefix_buf =
                        create_device_buffer(counts_bytes, kPhaseBBufferUsage);
                    Buffer batch_blocks_buf =
                        create_device_buffer(blocks_bytes, kPhaseBBufferUsage);
                    Buffer batch_qrec_buf =
                        create_device_buffer(dense_qrec_bytes, kPhaseBBufferUsage);
                    if (batch_counts_buf.mapped == nullptr
                        || batch_prefix_buf.mapped == nullptr
                        || batch_blocks_buf.mapped == nullptr
                        || batch_qrec_buf.mapped == nullptr) {
                        throw VulkanRuntimeError(
                            "phase_c seed-to-coordinate multi-record batch non-unified memory");
                    }
                    std::memset(
                        batch_counts_buf.mapped, 0,
                        static_cast<std::size_t>(batch_counts_buf.bytes));
                    std::memset(
                        batch_prefix_buf.mapped, 0,
                        static_cast<std::size_t>(batch_prefix_buf.bytes));
                    std::memset(
                        batch_blocks_buf.mapped, 0,
                        static_cast<std::size_t>(batch_blocks_buf.bytes));
                    std::memset(
                        batch_qrec_buf.mapped, 0,
                        static_cast<std::size_t>(batch_qrec_buf.bytes));
                    for (std::size_t active_index = 0;
                         active_index < active_states.size();
                         ++active_index) {
                        std::memcpy(
                            static_cast<std::byte*>(batch_qrec_buf.mapped)
                                + active_index * sizeof(PhaseCSeedQueryRecordWire),
                            &active_states[active_index]->query_record,
                            sizeof(PhaseCSeedQueryRecordWire));
                    }

                    SeedPushC batch_seed_push = first_active->seed_push;
                    batch_seed_push.record_count =
                        static_cast<std::uint32_t>(active_states.size());
                    batch_seed_push.emit_mode = 0u;
                    const std::array<VkDescriptorBufferInfo, 8> count_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {first_active->job.slots->buffer, 0, VK_WHOLE_SIZE},
                        {first_active->job.positions->buffer, 0, VK_WHOLE_SIZE},
                        {batch_qrec_buf.buffer, 0, batch_qrec_buf.bytes},
                        {batch_counts_buf.buffer, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer, 0, batch_prefix_buf.bytes},
                        {batch_counts_buf.buffer, 0, batch_counts_buf.bytes},
                        {batch_counts_buf.buffer, 0, batch_counts_buf.bytes},
                    }};
                    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                        {batch_counts_buf.buffer, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer, 0, batch_prefix_buf.bytes},
                        {batch_blocks_buf.buffer, 0, batch_blocks_buf.bytes},
                    }};
                    ScanPushC scan{
                        total_mc, kPhaseCScanBlock, total_blocks, 0u};

                    reset_phase_c_pool_locked();
                    check_vk(
                        vkResetCommandBuffer(command_buffer_, 0),
                        "reset(pc_seed_coord_mrec1)");
                    VkCommandBufferBeginInfo begin{};
                    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    check_vk(
                        vkBeginCommandBuffer(command_buffer_, &begin),
                        "begin(pc_seed_coord_mrec1)");
                    record_host_to_shader(command_buffer_);
                    record_phase_c_dispatch_locked(
                        command_buffer_, count_pipe, count_infos,
                        &batch_seed_push, sizeof(batch_seed_push),
                        static_cast<std::uint32_t>(active_states.size()));
                    run.stats.seed_count_dispatches += 1u;
                    record_compute_barrier(command_buffer_);
                    record_phase_c_dispatch_locked(
                        command_buffer_, scanb, scan_infos,
                        &scan, sizeof(scan), total_blocks);
                    record_compute_barrier(command_buffer_);
                    record_phase_c_dispatch_locked(
                        command_buffer_, scana, scan_infos,
                        &scan, sizeof(scan), 1u);
                    record_compute_barrier(command_buffer_);
                    scan.pass = 1u;
                    record_phase_c_dispatch_locked(
                        command_buffer_, scana, scan_infos,
                        &scan, sizeof(scan), ceil_div_u32(total_mc, 128u));
                    record_shader_to_host(command_buffer_);
                    submit_and_wait(
                        device_, queue_, command_buffer_, fence_,
                        "submit(pc_seed_coord_mrec1)");

                    std::uint32_t wave_total_prefix = 0u;
                    std::uint32_t wave_total_count = 0u;
                    std::memcpy(
                        &wave_total_prefix,
                        static_cast<std::byte*>(batch_prefix_buf.mapped)
                            + std::uint64_t{total_mc - 1u} * 4u,
                        4u);
                    std::memcpy(
                        &wave_total_count,
                        static_cast<std::byte*>(batch_counts_buf.mapped)
                            + std::uint64_t{total_mc - 1u} * 4u,
                        4u);
                    const std::uint64_t wave_total_anchors =
                        static_cast<std::uint64_t>(wave_total_prefix)
                        + wave_total_count;
                    if (wave_total_anchors > std::numeric_limits<std::uint32_t>::max()) {
                        throw VulkanRuntimeError(
                            "phase_c seed-to-coordinate multi-record anchor overflow");
                    }

                    std::uint32_t anchor_active_count = 0;
                    for (PhaseCSeedCoordinateWaveState* state : active_states) {
                        const std::uint32_t begin_min =
                            state->query_record.minimizer_offset;
                        const std::uint32_t end_min =
                            begin_min + state->query_record.minimizer_count - 1u;
                        std::uint32_t anchor_begin = 0u;
                        std::uint32_t anchor_end_prefix = 0u;
                        std::uint32_t anchor_end_count = 0u;
                        std::memcpy(
                            &anchor_begin,
                            static_cast<std::byte*>(batch_prefix_buf.mapped)
                                + std::uint64_t{begin_min} * 4u,
                            4u);
                        std::memcpy(
                            &anchor_end_prefix,
                            static_cast<std::byte*>(batch_prefix_buf.mapped)
                                + std::uint64_t{end_min} * 4u,
                            4u);
                        std::memcpy(
                            &anchor_end_count,
                            static_cast<std::byte*>(batch_counts_buf.mapped)
                                + std::uint64_t{end_min} * 4u,
                            4u);
                        const std::uint64_t anchor_end =
                            static_cast<std::uint64_t>(anchor_end_prefix)
                            + anchor_end_count;
                        if (anchor_end < anchor_begin) {
                            throw VulkanRuntimeError(
                                "phase_c seed-to-coordinate multi-record invalid prefix");
                        }
                        state->total_anchors =
                            anchor_end - static_cast<std::uint64_t>(anchor_begin);
                        if (state->total_anchors == 0u) {
                            continue;
                        }
                        if (state->total_anchors
                            > std::numeric_limits<std::uint32_t>::max()) {
                            throw VulkanRuntimeError(
                                "phase_c seed-to-coordinate multi-record per-query overflow");
                        }
                        state->total_anchors_u32 =
                            static_cast<std::uint32_t>(state->total_anchors);
                        state->anchor_offset = anchor_begin;
                        state->has_anchors = true;
                        anchor_active_count += 1u;
                    }

                    if (anchor_active_count == 0u || wave_total_anchors == 0u) {
                        arena.rewind(mark);
                        submitted_waves += 1u;
                        run.stats.wave_count += 1u;
                        wave_begin = next_wave_begin;
                        continue;
                    }

                    const std::uint64_t anchor_bytes =
                        wave_total_anchors * 3u * sizeof(std::uint64_t);
                    Buffer batch_anchors_buf =
                        create_device_buffer(anchor_bytes, kPhaseBBufferUsage);
                    Buffer batch_anchor_keys_buf = create_device_buffer(
                        wave_total_anchors * sizeof(std::uint64_t),
                        kPhaseBBufferUsage);
                    if (batch_anchors_buf.mapped == nullptr
                        || batch_anchor_keys_buf.mapped == nullptr) {
                        throw VulkanRuntimeError(
                            "phase_c seed-to-coordinate multi-record anchor memory");
                    }
                    std::memset(
                        batch_anchors_buf.mapped, 0,
                        static_cast<std::size_t>(batch_anchors_buf.bytes));
                    std::memset(
                        batch_anchor_keys_buf.mapped, 0,
                        static_cast<std::size_t>(batch_anchor_keys_buf.bytes));

                    std::vector<PhaseCCoordinateVoteJobWire> vote_jobs;
                    vote_jobs.reserve(anchor_active_count);
                    for (PhaseCSeedCoordinateWaveState* state : active_states) {
                        if (!state->has_anchors) {
                            continue;
                        }
                        state->vote_result_index =
                            static_cast<std::uint32_t>(vote_jobs.size());
                        vote_jobs.push_back(PhaseCCoordinateVoteJobWire{
                            .anchor_offset = state->anchor_offset,
                            .anchor_count = state->total_anchors_u32,
                            .query_len_lo = static_cast<std::uint32_t>(
                                state->query_len & 0xffffffffull),
                            .query_len_hi = static_cast<std::uint32_t>(
                                state->query_len >> 32),
                        });
                    }
                    const std::uint64_t vote_job_bytes =
                        std::max<std::uint64_t>(
                            static_cast<std::uint64_t>(vote_jobs.size())
                                * sizeof(PhaseCCoordinateVoteJobWire),
                            sizeof(PhaseCCoordinateVoteJobWire));
                    const std::uint64_t vote_result_bytes =
                        std::max<std::uint64_t>(
                            static_cast<std::uint64_t>(vote_jobs.size())
                                * sizeof(GpuCoordinateOnlyResultWire),
                            sizeof(GpuCoordinateOnlyResultWire));
                    Buffer vote_jobs_buf =
                        create_device_buffer(vote_job_bytes, kPhaseBBufferUsage);
                    Buffer vote_results_buf =
                        create_device_buffer(vote_result_bytes, kPhaseBBufferUsage);
                    if (vote_jobs_buf.mapped == nullptr
                        || vote_results_buf.mapped == nullptr) {
                        throw VulkanRuntimeError(
                            "phase_c seed-to-coordinate multi-record vote memory");
                    }
                    std::memset(
                        vote_jobs_buf.mapped, 0,
                        static_cast<std::size_t>(vote_jobs_buf.bytes));
                    std::memset(
                        vote_results_buf.mapped, 0,
                        static_cast<std::size_t>(vote_results_buf.bytes));
                    std::memcpy(
                        vote_jobs_buf.mapped, vote_jobs.data(),
                        vote_jobs.size() * sizeof(PhaseCCoordinateVoteJobWire));

                    reset_phase_c_pool_locked();
                    check_vk(
                        vkResetCommandBuffer(command_buffer_, 0),
                        "reset(pc_seed_coord_mrec2)");
                    begin = VkCommandBufferBeginInfo{};
                    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    check_vk(
                        vkBeginCommandBuffer(command_buffer_, &begin),
                        "begin(pc_seed_coord_mrec2)");
                    record_host_to_shader(command_buffer_);
                    batch_seed_push.emit_mode = 1u;
                    const std::array<VkDescriptorBufferInfo, 8> emit_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {first_active->job.slots->buffer, 0, VK_WHOLE_SIZE},
                        {first_active->job.positions->buffer, 0, VK_WHOLE_SIZE},
                        {batch_qrec_buf.buffer, 0, batch_qrec_buf.bytes},
                        {batch_counts_buf.buffer, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer, 0, batch_prefix_buf.bytes},
                        {batch_anchors_buf.buffer, 0, batch_anchors_buf.bytes},
                        {batch_anchor_keys_buf.buffer, 0, batch_anchor_keys_buf.bytes},
                    }};
                    record_phase_c_dispatch_locked(
                        command_buffer_, emit_pipe, emit_infos,
                        &batch_seed_push, sizeof(batch_seed_push),
                        static_cast<std::uint32_t>(active_states.size()));
                    run.stats.seed_emit_dispatches += 1u;
                    record_compute_barrier(command_buffer_);
                    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                        {batch_anchors_buf.buffer, 0, batch_anchors_buf.bytes},
                        {vote_jobs_buf.buffer, 0, vote_jobs_buf.bytes},
                        {batch_anchor_keys_buf.buffer, 0, batch_anchor_keys_buf.bytes},
                        {vote_results_buf.buffer, 0, vote_results_buf.bytes},
                        {vote_results_buf.buffer, 0, vote_results_buf.bytes},
                        {vote_results_buf.buffer, 0, vote_results_buf.bytes},
                    }};
                    GpuCoordinateReducePush coord_push{
                        .anchor_count = static_cast<std::uint32_t>(vote_jobs.size()),
                        .query_len_lo = 0u,
                        .query_len_hi = 0u,
                        .min_chain_score = min_chain_score,
                        .min_seed_count = min_seed_count,
                        .probe_limit = probe_limit,
                        .reserved = 0u,
                    };
                    record_phase_c_dispatch_locked(
                        command_buffer_, vote_coord_batch_pipe, coord_infos,
                        &coord_push, sizeof(coord_push),
                        static_cast<std::uint32_t>(vote_jobs.size()));
                    run.stats.coordinate_vote_dispatches += 1u;
                    record_shader_to_host(command_buffer_);
                    submit_and_wait(
                        device_, queue_, command_buffer_, fence_,
                        "submit(pc_seed_coord_mrec2)");

                    for (PhaseCSeedCoordinateWaveState* state : active_states) {
                        if (state->has_anchors) {
                            std::memcpy(
                                &run.results[state->result_index],
                                static_cast<std::byte*>(vote_results_buf.mapped)
                                    + static_cast<std::uint64_t>(
                                        state->vote_result_index)
                                        * sizeof(GpuCoordinateOnlyResultWire),
                                sizeof(GpuCoordinateOnlyResultWire));
                        }
                    }

                    arena.rewind(mark);
                    submitted_waves += 1u;
                    run.stats.wave_count += 1u;
                    wave_begin = next_wave_begin;
                    continue;
                }
            }

            reset_phase_c_pool_locked();
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->burn_before_reset(command_buffer_);
            }
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch1)");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord_batch1)");
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->begin(command_buffer_);
            }
            record_host_to_shader(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                if (shape_v5_selected) {
                    const PhaseCShapeV5ResidentView& view =
                        *state.job.shape_v5;
                    const std::array<VkDescriptorBufferInfo, 9> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {view.descriptors->buffer, 0, view.descriptor_bytes},
                        {view.controls->buffer, 0, shape_control_padded_bytes},
                        {view.positions->buffer, 0, shape_position_padded_bytes},
                        {qrec_s.buffer->buffer,
                         qrec_s.offset + std::uint64_t{state.job.query_index}
                             * qrec_stride,
                         sizeof(PhaseCSeedQueryRecordWire)},
                        {state.counts_s.buffer->buffer,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {state.counts_s.buffer->buffer,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.counts_s.buffer->buffer,
                         state.counts_s.offset,
                         state.counts_s.size},
                    }};
                    record_phase_c_dispatch_locked(
                        command_buffer_, count_pipe, seed_infos,
                        &state.shape_v5_seed_push,
                        sizeof(state.shape_v5_seed_push),
                        1u, shape_pipeline_collector.get(),
                        state.result_index);
                    run.shape_v5_direct.count_dispatches += 1u;
                    state.operation_audit.direct_count_dispatches += 1u;
                } else {
                    const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {state.job.slots->buffer, 0, VK_WHOLE_SIZE},
                        {state.job.positions->buffer, 0, VK_WHOLE_SIZE},
                        {qrec_s.buffer->buffer,
                         qrec_s.offset + std::uint64_t{state.job.query_index}
                             * qrec_stride,
                         sizeof(PhaseCSeedQueryRecordWire)},
                        {state.counts_s.buffer->buffer,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {state.counts_s.buffer->buffer,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.counts_s.buffer->buffer,
                         state.counts_s.offset,
                         state.counts_s.size},
                    }};
                    record_phase_c_dispatch_locked(
                        command_buffer_, count_pipe, seed_infos,
                        &state.seed_push, sizeof(state.seed_push),
                        state.seed_groups);
                }
                run.stats.seed_count_dispatches += 1u;
            }
            record_compute_barrier(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer, state.prefix_s.offset, state.prefix_s.size},
                    {state.blocks_s.buffer->buffer, state.blocks_s.offset, state.blocks_s.size},
                }};
                ScanPushC scan{
                    state.minimizer_count, kPhaseCScanBlock, state.num_blocks, 0u};
                record_phase_c_dispatch_locked(
                    command_buffer_, scanb, scan_infos, &scan, sizeof(scan),
                    state.num_blocks, shape_pipeline_collector.get(),
                    state.result_index);
                if (shape_v5_selected) {
                    state.operation_audit.direct_scan_dispatches += 1u;
                }
            }
            record_compute_barrier(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer, state.prefix_s.offset, state.prefix_s.size},
                    {state.blocks_s.buffer->buffer, state.blocks_s.offset, state.blocks_s.size},
                }};
                ScanPushC scan{
                    state.minimizer_count, kPhaseCScanBlock, state.num_blocks, 0u};
                record_phase_c_dispatch_locked(
                    command_buffer_, scana, scan_infos, &scan, sizeof(scan),
                    1u, shape_pipeline_collector.get(), state.result_index);
                if (shape_v5_selected) {
                    state.operation_audit.direct_scan_dispatches += 1u;
                }
            }
            record_compute_barrier(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer, state.prefix_s.offset, state.prefix_s.size},
                    {state.blocks_s.buffer->buffer, state.blocks_s.offset, state.blocks_s.size},
                }};
                ScanPushC scan{
                    state.minimizer_count, kPhaseCScanBlock, state.num_blocks, 1u};
                record_phase_c_dispatch_locked(
                    command_buffer_, scana, scan_infos, &scan, sizeof(scan),
                    ceil_div_u32(state.minimizer_count, 128u),
                    shape_pipeline_collector.get(), state.result_index);
                if (shape_v5_selected) {
                    state.operation_audit.direct_scan_dispatches += 1u;
                }
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(
                device_, queue_, command_buffer_, fence_,
                "submit(pc_seed_coord_batch1)",
                shape_pipeline_collector.get());
            if (shape_v5_selected) {
                const std::uint64_t receipt_identity =
                    next_operation_proof_identity_locked();
                for (PhaseCSeedCoordinateWaveState& state : states) {
                    if (!state.skip_seed) {
                        state.operation_audit.count_submit_wait_identity =
                            receipt_identity;
                    }
                }
            }

            std::uint32_t anchor_active_count = 0;
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                if (shape_v5_selected) {
                    std::uint32_t status = 0u;
                    std::memcpy(
                        &status,
                        base + state.counts_s.offset
                            + static_cast<std::uint64_t>(state.minimizer_count)
                                * sizeof(std::uint32_t),
                        sizeof(status));
                    run.shape_v5_direct.count_status_checks += 1u;
                    run.shape_v5_direct.status_or |= status;
                    state.operation_audit.count_status_checks += 1u;
                    state.operation_audit.status_or |= status;
                    if (status != 0u) {
                        throw VulkanRuntimeError(fmt::format(
                            "phase_c Shape Format-5 count rejected job {} with status 0x{:08x}",
                            state.result_index,
                            status));
                    }
                }
                std::uint32_t plast = 0;
                std::uint32_t clast = 0;
                std::memcpy(
                    &plast,
                    base + state.prefix_s.offset
                        + std::uint64_t{state.minimizer_count - 1u} * 4u,
                    4u);
                std::memcpy(
                    &clast,
                    base + state.counts_s.offset
                        + std::uint64_t{state.minimizer_count - 1u} * 4u,
                    4u);
                state.total_anchors = static_cast<std::uint64_t>(plast) + clast;
                if (shape_v5_selected) {
                    state.operation_audit.anchor_count =
                        state.total_anchors;
                    shape_pipeline_collector->set_shape_outcome(
                        state.result_index, state.total_anchors, 0u);
                }
                if (state.total_anchors == 0u) {
                    if (shape_v5_selected) {
                        run.operation_proof.records_[state.result_index].audit =
                            state.operation_audit;
                    }
                    continue;
                }
                if (state.total_anchors > std::numeric_limits<std::uint32_t>::max()) {
                    throw VulkanRuntimeError(
                        "phase_c seed-to-coordinate batch anchor count overflow");
                }
                state.total_anchors_u32 = static_cast<std::uint32_t>(state.total_anchors);
                state.anchor_wg =
                    plan_workgroups(state.total_anchors, features_.max_workgroup_count_x);
                const std::uint64_t anchor_u64 = state.total_anchors * 3u;
                state.anchors_buf =
                    create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
                state.anchor_keys_buf =
                    create_device_buffer(state.total_anchors * 8u, kPhaseBBufferUsage);
                if (coordinate_vote_selected) {
                    state.result_buf = create_device_buffer(
                        sizeof(GpuCoordinateOnlyResultWire), kPhaseBBufferUsage);
                    if (state.anchors_buf.mapped == nullptr
                        || state.anchor_keys_buf.mapped == nullptr
                        || state.result_buf.mapped == nullptr) {
                        throw VulkanRuntimeError(
                            "phase_c seed-to-coordinate vote batch non-unified memory");
                    }
                    std::memset(
                        state.result_buf.mapped, 0,
                        static_cast<std::size_t>(state.result_buf.bytes));
                    state.has_anchors = true;
                    anchor_active_count += 1u;
                    continue;
                }
                state.anchors_scratch_buf =
                    create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
                state.anchor_keys_scratch_buf =
                    create_device_buffer(state.total_anchors * 8u, kPhaseBBufferUsage);
                state.anchor_histogram_buf = create_device_buffer(
                    std::uint64_t{state.anchor_wg.num_workgroups} * 256u * 4u,
                    kPhaseBBufferUsage);
                state.ranges_buf = create_device_buffer(
                    std::max<std::uint64_t>(
                        state.total_anchors * sizeof(GpuChainRangeWire), 8u),
                    kPhaseBBufferUsage);
                state.range_count_buf = create_device_buffer(8u, kPhaseBBufferUsage);
                state.compacted_anchors_buf =
                    create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
                state.span_buf =
                    create_device_buffer(sizeof(GpuChainRangeBuildSpanWire), kPhaseBBufferUsage);
                if (state.anchors_buf.mapped == nullptr
                    || state.anchor_keys_buf.mapped == nullptr
                    || state.anchors_scratch_buf.mapped == nullptr
                    || state.anchor_keys_scratch_buf.mapped == nullptr
                    || state.anchor_histogram_buf.mapped == nullptr
                    || state.ranges_buf.mapped == nullptr
                    || state.range_count_buf.mapped == nullptr
                    || state.compacted_anchors_buf.mapped == nullptr
                    || state.span_buf.mapped == nullptr) {
                    throw VulkanRuntimeError(
                        "phase_c seed-to-coordinate batch non-unified anchor memory");
                }
                const GpuChainRangeBuildSpanWire span{
                    .anchor_lo = 0u,
                    .anchor_hi = state.total_anchors_u32,
                    .range_lo = 0u,
                    .range_out = 0u,
                };
                std::memcpy(state.span_buf.mapped, &span, sizeof(span));
                std::memset(state.range_count_buf.mapped, 0, 8u);
                state.has_anchors = true;
                anchor_active_count += 1u;
            }

            if (anchor_active_count == 0u) {
                arena.rewind(mark);
                submitted_waves += 1u;
                run.stats.wave_count += 1u;
                wave_begin = next_wave_begin;
                continue;
            }

            reset_phase_c_pool_locked();
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->burn_before_reset(command_buffer_);
            }
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch2)");
            begin = VkCommandBufferBeginInfo{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord_batch2)");
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->begin(command_buffer_);
            }
            record_host_to_shader(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                GpuArenaSlice anchors_s = state.anchors_slice();
                GpuArenaSlice anchor_keys_s = state.anchor_keys_slice();
                if (shape_v5_selected) {
                    GpuShapeV5SeedPush emit_push = state.shape_v5_seed_push;
                    emit_push.emit_mode = 1u;
                    emit_push.anchor_capacity = state.total_anchors_u32;
                    const PhaseCShapeV5ResidentView& view =
                        *state.job.shape_v5;
                    const std::array<VkDescriptorBufferInfo, 9> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {view.descriptors->buffer, 0, view.descriptor_bytes},
                        {view.controls->buffer, 0, shape_control_padded_bytes},
                        {view.positions->buffer, 0, shape_position_padded_bytes},
                        {qrec_s.buffer->buffer,
                         qrec_s.offset + std::uint64_t{state.job.query_index}
                             * qrec_stride,
                         sizeof(PhaseCSeedQueryRecordWire)},
                        {state.counts_s.buffer->buffer,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {anchors_s.buffer->buffer,
                         anchors_s.offset,
                         anchors_s.size},
                        {anchor_keys_s.buffer->buffer,
                         anchor_keys_s.offset,
                         anchor_keys_s.size},
                    }};
                    record_phase_c_dispatch_locked(
                        command_buffer_, emit_pipe, seed_infos,
                        &emit_push, sizeof(emit_push), 1u,
                        shape_pipeline_collector.get(), state.result_index);
                    run.shape_v5_direct.emit_dispatches += 1u;
                    state.operation_audit.direct_emit_dispatches += 1u;
                } else {
                    SeedPushC emit_push = state.seed_push;
                    emit_push.emit_mode = 1u;
                    const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {state.job.slots->buffer, 0, VK_WHOLE_SIZE},
                        {state.job.positions->buffer, 0, VK_WHOLE_SIZE},
                        {qrec_s.buffer->buffer,
                         qrec_s.offset + std::uint64_t{state.job.query_index}
                             * qrec_stride,
                         sizeof(PhaseCSeedQueryRecordWire)},
                        {state.counts_s.buffer->buffer,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {anchors_s.buffer->buffer,
                         anchors_s.offset,
                         anchors_s.size},
                        {anchor_keys_s.buffer->buffer,
                         anchor_keys_s.offset,
                         anchor_keys_s.size},
                    }};
                    record_phase_c_dispatch_locked(
                        command_buffer_, emit_pipe, seed_infos,
                        &emit_push, sizeof(emit_push), state.seed_groups);
                }
                run.stats.seed_emit_dispatches += 1u;
            }
            record_compute_barrier(command_buffer_);
            if (shape_v5_selected) {
                record_shader_to_host(command_buffer_);
                submit_and_wait(
                    device_, queue_, command_buffer_, fence_,
                    "submit(pc_shape_v5_seed_coord_emit)",
                    shape_pipeline_collector.get());
                const std::uint64_t receipt_identity =
                    next_operation_proof_identity_locked();
                for (PhaseCSeedCoordinateWaveState& state : states) {
                    if (!state.has_anchors) {
                        continue;
                    }
                    state.operation_audit.emit_submit_wait_identity =
                        receipt_identity;
                    std::uint32_t status = 0u;
                    std::memcpy(
                        &status,
                        base + state.counts_s.offset
                            + static_cast<std::uint64_t>(state.minimizer_count)
                                * sizeof(std::uint32_t),
                        sizeof(status));
                    run.shape_v5_direct.emit_status_checks += 1u;
                    run.shape_v5_direct.status_or |= status;
                    state.operation_audit.emit_status_checks += 1u;
                    state.operation_audit.status_or |= status;
                    if (status != 0u) {
                        throw VulkanRuntimeError(fmt::format(
                            "phase_c Shape Format-5 emit rejected job {} with status 0x{:08x}",
                            state.result_index,
                            status));
                    }
                }

                reset_phase_c_pool_locked();
                shape_pipeline_collector->burn_before_reset(command_buffer_);
                check_vk(
                    vkResetCommandBuffer(command_buffer_, 0),
                    "reset(pc_shape_v5_seed_coord_post_emit)");
                begin = VkCommandBufferBeginInfo{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                check_vk(
                    vkBeginCommandBuffer(command_buffer_, &begin),
                    "begin(pc_shape_v5_seed_coord_post_emit)");
                shape_pipeline_collector->begin(command_buffer_);
            }
            if (coordinate_vote_selected) {
                for (PhaseCSeedCoordinateWaveState& state : states) {
                    if (!state.has_anchors) {
                        continue;
                    }
                    GpuArenaSlice anchors_s = state.anchors_slice();
                    GpuArenaSlice anchor_keys_s = state.anchor_keys_slice();
                    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                        {anchors_s.buffer->buffer, anchors_s.offset, anchors_s.size},
                        {anchor_keys_s.buffer->buffer, anchor_keys_s.offset, anchor_keys_s.size},
                        {anchor_keys_s.buffer->buffer, anchor_keys_s.offset, anchor_keys_s.size},
                        {state.result_buf.buffer, 0, state.result_buf.bytes},
                        {state.result_buf.buffer, 0, state.result_buf.bytes},
                        {state.result_buf.buffer, 0, state.result_buf.bytes},
                    }};
                    GpuCoordinateReducePush coord_push{
                        .anchor_count = state.total_anchors_u32,
                        .query_len_lo = static_cast<std::uint32_t>(
                            state.query_len & 0xffffffffull),
                        .query_len_hi = static_cast<std::uint32_t>(
                            state.query_len >> 32),
                        .min_chain_score = min_chain_score,
                        .min_seed_count = min_seed_count,
                        .probe_limit = probe_limit,
                        .reserved = 0u,
                    };
                    record_phase_c_dispatch_locked(
                        command_buffer_, vote_coord_pipe, coord_infos,
                        &coord_push, sizeof(coord_push), 1u);
                    run.stats.coordinate_vote_dispatches += 1u;
                }
                record_shader_to_host(command_buffer_);
                submit_and_wait(
                    device_, queue_, command_buffer_, fence_,
                    "submit(pc_seed_coord_vote_batch2)");

                for (PhaseCSeedCoordinateWaveState& state : states) {
                    if (state.has_anchors) {
                        std::memcpy(
                            &run.results[state.result_index],
                            state.result_buf.mapped,
                            sizeof(GpuCoordinateOnlyResultWire));
                    }
                }

                arena.rewind(mark);
                submitted_waves += 1u;
                run.stats.wave_count += 1u;
                wave_begin = next_wave_begin;
                continue;
            }
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                GpuArenaSlice anchors_s = state.anchors_slice();
                GpuArenaSlice anchor_keys_s = state.anchor_keys_slice();
                GpuArenaSlice anchors_scratch_s = state.anchors_scratch_slice();
                GpuArenaSlice anchor_keys_scratch_s = state.anchor_keys_scratch_slice();
                GpuArenaSlice anchor_histogram_s = state.anchor_histogram_slice();
                const PhaseCAnchorSortKeyPlan anchor_sort_plan =
                    phase_c_anchor_sort_key_plan(
                        state.job.target_len,
                        state.job.target_position_offset);
                GpuSortPlan sort_plan{
                    state.total_anchors, 8u, 24u, 8u, anchor_sort_plan.pass_count};
                record_radix_sort_locked(
                    command_buffer_,
                    anchor_keys_s,
                    anchors_s,
                    anchor_keys_scratch_s,
                    anchors_scratch_s,
                    anchor_histogram_s,
                    sort_plan,
                    state.anchor_wg.num_workgroups,
                    state.anchor_wg.elems_per_wg,
                    3u,
                    state.sorted_in_scratch,
                    shape_pipeline_collector.get(), state.result_index);
                run.stats.sort_dispatches += 1u;
                if (shape_v5_selected) {
                    state.operation_audit.sort_operations += 1u;
                }
                const GpuArenaSlice sorted_anchors =
                    state.sorted_in_scratch ? anchors_scratch_s : anchors_s;
                const GpuArenaSlice compacted_anchors_s = state.compacted_anchors_slice();
                const std::array<VkDescriptorBufferInfo, 6> range_infos{{
                    {sorted_anchors.buffer->buffer, sorted_anchors.offset, sorted_anchors.size},
                    {state.range_count_buf.buffer, 0, state.range_count_buf.bytes},
                    {state.ranges_buf.buffer, 0, state.ranges_buf.bytes},
                    {state.range_count_buf.buffer, 0, state.range_count_buf.bytes},
                    {compacted_anchors_s.buffer->buffer, compacted_anchors_s.offset,
                     compacted_anchors_s.size},
                    {state.span_buf.buffer, 0, state.span_buf.bytes},
                }};
                ChainRangePushLocal range_push{
                    .query_count = 1u,
                    .max_ref_gap =
                        static_cast<std::uint32_t>(std::max<std::int32_t>(max_dist_x, 0)),
                };
                record_phase_c_dispatch_locked(
                    command_buffer_, range_pipe, range_infos,
                    &range_push, sizeof(range_push), 1u,
                    shape_pipeline_collector.get(), state.result_index);
                run.stats.range_build_dispatches += 1u;
                if (shape_v5_selected) {
                    state.operation_audit.range_build_dispatches += 1u;
                }
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(
                device_, queue_, command_buffer_, fence_,
                "submit(pc_seed_coord_batch2)",
                shape_pipeline_collector.get());
            if (shape_v5_selected) {
                const std::uint64_t receipt_identity =
                    next_operation_proof_identity_locked();
                for (PhaseCSeedCoordinateWaveState& state : states) {
                    if (state.has_anchors) {
                        state.operation_audit.sort_range_submit_wait_identity =
                            receipt_identity;
                    }
                }
            }

            std::uint32_t chain_active_count = 0;
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                std::memcpy(&state.range_count, state.range_count_buf.mapped, 4u);
                std::memcpy(
                    &state.compacted_anchor_count,
                    static_cast<std::byte*>(state.range_count_buf.mapped) + 4u,
                    4u);
                if (shape_v5_selected) {
                    state.operation_audit.range_count = state.range_count;
                    shape_pipeline_collector->set_shape_outcome(
                        state.result_index,
                        state.total_anchors,
                        state.range_count);
                }
                if (state.range_count == 0u) {
                    if (shape_v5_selected) {
                        run.operation_proof.records_[state.result_index].audit =
                            state.operation_audit;
                    }
                    continue;
                }
                if (state.compacted_anchor_count > state.total_anchors_u32) {
                    throw VulkanRuntimeError(
                        "phase_c seed-to-coordinate batch compacted anchor overflow");
                }
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u
                    && state.compacted_anchor_count < state.total_anchors_u32;
                state.active_anchor_count =
                    use_compacted_anchors ? state.compacted_anchor_count
                                          : state.total_anchors_u32;
                state.score_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::int32_t),
                    kPhaseBBufferUsage);
                state.predecessor_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::int64_t),
                    kPhaseBBufferUsage);
                state.visited_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::uint32_t),
                    kPhaseBBufferUsage);
                state.trace_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::uint32_t),
                    kPhaseBBufferUsage);
                state.result_buf =
                    create_device_buffer(sizeof(GpuCoordinateOnlyResultWire), kPhaseBBufferUsage);
                if (state.score_buf.mapped == nullptr
                    || state.predecessor_buf.mapped == nullptr
                    || state.visited_buf.mapped == nullptr
                    || state.trace_buf.mapped == nullptr
                    || state.result_buf.mapped == nullptr) {
                    throw VulkanRuntimeError(
                        "phase_c seed-to-coordinate batch non-unified chain memory");
                }
                state.chain_active = true;
                chain_active_count += 1u;
            }

            if (chain_active_count == 0u) {
                arena.rewind(mark);
                submitted_waves += 1u;
                run.stats.wave_count += 1u;
                wave_begin = next_wave_begin;
                continue;
            }

            reset_phase_c_pool_locked();
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->burn_before_reset(command_buffer_);
            }
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch3)");
            begin = VkCommandBufferBeginInfo{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord_batch3)");
            if (shape_pipeline_collector != nullptr) {
                shape_pipeline_collector->begin(command_buffer_);
            }
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                std::memset(
                    state.score_buf.mapped, 0,
                    static_cast<std::size_t>(state.score_buf.bytes));
                std::memset(
                    state.predecessor_buf.mapped, 0xff,
                    static_cast<std::size_t>(state.predecessor_buf.bytes));
                std::memset(
                    state.visited_buf.mapped, 0,
                    static_cast<std::size_t>(state.visited_buf.bytes));
                std::memset(
                    state.result_buf.mapped, 0,
                    static_cast<std::size_t>(state.result_buf.bytes));
            }
            record_host_to_shader(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                GpuArenaSlice anchors_s = state.anchors_slice();
                GpuArenaSlice anchors_scratch_s = state.anchors_scratch_slice();
                GpuArenaSlice compacted_anchors_s = state.compacted_anchors_slice();
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u
                    && state.compacted_anchor_count < state.total_anchors_u32;
                const GpuArenaSlice active_anchors = use_compacted_anchors
                    ? compacted_anchors_s
                    : (state.sorted_in_scratch ? anchors_scratch_s : anchors_s);
                const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
                    {active_anchors.buffer->buffer, active_anchors.offset, active_anchors.size},
                    {state.ranges_buf.buffer, 0, state.ranges_buf.bytes},
                    {state.score_buf.buffer, 0, state.score_buf.bytes},
                    {state.predecessor_buf.buffer, 0, state.predecessor_buf.bytes},
                }};
                VkDescriptorSet chain_set = alloc_phase_c_set_locked(descriptor_set_layout_);
                std::array<VkWriteDescriptorSet, 4> writes{};
                for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
                    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[binding].dstSet = chain_set;
                    writes[binding].dstBinding = binding;
                    writes[binding].descriptorCount = 1;
                    writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[binding].pBufferInfo = &chain_infos[binding];
                }
                vkUpdateDescriptorSets(
                    device_, static_cast<std::uint32_t>(writes.size()),
                    writes.data(), 0, nullptr);

                ChainingPushConstants chain_push{};
                chain_push.anchor_count = state.active_anchor_count;
                chain_push.range_count = state.range_count;
                chain_push.max_dist_x = max_dist_x;
                chain_push.max_dist_y = max_dist_y;
                chain_push.bandwidth = bandwidth;
                chain_push.max_iter = max_iter;
                chain_push.gap_penalty = gap_penalty;
                chain_push.skip_penalty = skip_penalty;
                chain_push.max_chain_skip = max_chain_skip;
                const std::uint32_t chain_groups =
                    std::min(state.range_count, features_.max_workgroup_count_x);
                vkCmdBindPipeline(
                    command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    chain_pipeline.pipeline);
                vkCmdBindDescriptorSets(
                    command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_layout_, 0, 1, &chain_set, 0, nullptr);
                vkCmdPushConstants(
                    command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(chain_push), &chain_push);
                if (shape_pipeline_collector != nullptr) {
                    shape_pipeline_collector->use(
                        command_buffer_, state.result_index,
                        chain_pipeline.shader_key,
                        chain_pipeline.metal_archive_receipt,
                        chain_groups);
                }
                vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
                run.stats.chain_dispatches += 1u;
                if (shape_v5_selected) {
                    state.operation_audit.chain_dispatches += 1u;
                }
            }
            record_compute_barrier(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                GpuArenaSlice anchors_s = state.anchors_slice();
                GpuArenaSlice anchors_scratch_s = state.anchors_scratch_slice();
                GpuArenaSlice compacted_anchors_s = state.compacted_anchors_slice();
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u
                    && state.compacted_anchor_count < state.total_anchors_u32;
                const GpuArenaSlice active_anchors = use_compacted_anchors
                    ? compacted_anchors_s
                    : (state.sorted_in_scratch ? anchors_scratch_s : anchors_s);
                const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                    {active_anchors.buffer->buffer, active_anchors.offset, active_anchors.size},
                    {state.score_buf.buffer, 0, state.score_buf.bytes},
                    {state.predecessor_buf.buffer, 0, state.predecessor_buf.bytes},
                    {state.visited_buf.buffer, 0, state.visited_buf.bytes},
                    {state.trace_buf.buffer, 0, state.trace_buf.bytes},
                    {state.result_buf.buffer, 0, state.result_buf.bytes},
                }};
                GpuCoordinateReducePush coord_push{
                    .anchor_count = state.active_anchor_count,
                    .query_len_lo = static_cast<std::uint32_t>(state.query_len & 0xffffffffull),
                    .query_len_hi = static_cast<std::uint32_t>(state.query_len >> 32),
                    .min_chain_score = min_chain_score,
                    .min_seed_count = min_seed_count,
                    .probe_limit = probe_limit,
                    .reserved = 0u,
                };
                record_phase_c_dispatch_locked(
                    command_buffer_, coord_pipe, coord_infos,
                    &coord_push, sizeof(coord_push), 1u,
                    shape_pipeline_collector.get(), state.result_index);
                run.stats.chain_backtrack_dispatches += 1u;
                if (shape_v5_selected) {
                    state.operation_audit.chain_backtrack_dispatches += 1u;
                    state.operation_audit.coordinate_dispatches += 1u;
                }
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(
                device_, queue_, command_buffer_, fence_,
                "submit(pc_seed_coord_batch3)",
                shape_pipeline_collector.get());
            if (shape_v5_selected) {
                const std::uint64_t receipt_identity =
                    next_operation_proof_identity_locked();
                for (PhaseCSeedCoordinateWaveState& state : states) {
                    if (state.chain_active) {
                        state.operation_audit
                            .chain_coordinate_submit_wait_identity =
                            receipt_identity;
                    }
                }
            }

            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.chain_active) {
                    std::memcpy(
                        &run.results[state.result_index],
                        state.result_buf.mapped,
                        sizeof(GpuCoordinateOnlyResultWire));
                    if (shape_v5_selected) {
                        state.operation_audit.result =
                            run.results[state.result_index];
                        state.operation_audit.result_readbacks += 1u;
                        run.operation_proof.records_[state.result_index].audit =
                            state.operation_audit;
                    }
                }
            }

            arena.rewind(mark);
            submitted_waves += 1u;
            run.stats.wave_count += 1u;
            wave_begin = next_wave_begin;
        } catch (...) {
            arena.rewind(mark);
            throw;
        }
    }

    if (shape_pipeline_collector != nullptr) {
        shape_pipeline_collector->burn_before_reset(command_buffer_);
    }
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch_post)");
    if (submitted_waves != 0u) {
        chaining_dispatch_count_.fetch_add(submitted_waves, std::memory_order_relaxed);
    }
    if (shape_v5_selected) {
        run.shape_v5_direct.direct_count_scan_emit_completed = true;
        run.operation_proof.query_pipeline_proof_ =
            std::move(query_authority.pipeline_proof_);
        run.operation_proof.shape_pipeline_proof_ =
            shape_pipeline_collector->seal();
        run.operation_proof.sealed_ =
            run.operation_proof.records_.size() == jobs.size()
            && !run.operation_proof.records_.empty()
            && run.operation_proof.causal_proofs_accepted()
            && std::all_of(
                run.operation_proof.records_.begin(),
                run.operation_proof.records_.end(),
                [](const auto& record) {
                    return record.state ==
                            PersistedShapeV5CoordinateOperationProof::
                                RecordState::fresh
                        && record.audit.accepted();
                });
    }
    return run;
}

runtime_detail::PersistedShapeV5CoordinateFinalizationPlan
Runtime::prepare_persisted_shape_v5_coordinate_finalization(
    const PersistedShapeV5CoordinateOperationProof& proof,
    const PersistedShapeV5CoordinateOperationEvidencePreview&
        expected_preview,
    const VulkanFeatureSet& live_features,
    const bool seed_metrics_claimed) noexcept {
    runtime_detail::PersistedShapeV5CoordinateFinalizationPlan result{};
    const PersistedShapeV5CoordinateOperationEvidencePreview proof_preview =
        proof.operation_evidence_preview();
    if (!proof.all_jobs_accepted() || !expected_preview.accepted()
        || !proof_preview.accepted()
        || proof_preview != expected_preview) {
        return result;
    }
    const RuntimeGpuExecutionEnvironmentBinding environment_preview =
        proof.environment_preview();
    const RuntimeGpuCacheMetricsAudit cache_metrics_maximum =
        proof.cache_metrics_preview();
    if (!environment_preview.accepted()
        || !cache_metrics_maximum.accepted()) {
        return result;
    }
    const RuntimePipelineOperationAudit& query =
        proof.query_pipeline_proof_.audit();
    const RuntimePipelineOperationAudit& shape =
        proof.shape_pipeline_proof_.audit();
    const bool environment_equal = query.device_uuid == shape.device_uuid
        && query.api_version == shape.api_version
        && query.driver_version == shape.driver_version
        && query.vendor_id == shape.vendor_id
        && query.device_id == shape.device_id
        && query.pipeline_cache_uuid == shape.pipeline_cache_uuid
        && query.seed_sha256 == shape.seed_sha256
        && query.seed_size == shape.seed_size
        && query.manifest_checksum == shape.manifest_checksum
        && query.seed_receipt_identity == shape.seed_receipt_identity
        && query.archive_set == shape.archive_set
        && query.device_uuid == live_features.device_uuid
        && query.api_version == live_features.api_version
        && query.driver_version == live_features.driver_version
        && query.vendor_id == live_features.vendor_id
        && query.device_id == live_features.device_id
        && query.pipeline_cache_uuid
            == live_features.pipeline_cache_uuid;
    if (!environment_equal) return result;

    const auto union_receipts = proof.receipt_union();
    if (!union_receipts.valid) return result;
    result.cache_claim_plan = runtime_detail::
        RuntimeGpuCacheMetricsClaimPlan::prepare(
            proof.operation_identity_, cache_metrics_maximum,
            seed_metrics_claimed,
            std::span<const RuntimeMetalArchivePipelineReceipt* const>{
                union_receipts.receipts.data(), union_receipts.count});
    if (!result.cache_claim_plan.accepted()) return {};

    VulkanFeatureSet identity_features{};
    identity_features.api_version = query.api_version;
    identity_features.driver_version = query.driver_version;
    identity_features.vendor_id = query.vendor_id;
    identity_features.device_id = query.device_id;
    identity_features.pipeline_cache_uuid = query.pipeline_cache_uuid;
    result.environment = RuntimeGpuExecutionEnvironmentBinding{
        .gpu_environment_present = true,
        .gpu_device_uuid = query.device_uuid,
        .gpu_driver_identity =
            canonical_vulkan_driver_identity(identity_features),
        .cache_policy = RuntimePipelineCachePolicyEvidence::
            immutable_preseed_all_hit,
        .seed_sha256 = query.seed_sha256,
        .seed_size = query.seed_size,
        .manifest_checksum = query.manifest_checksum,
        .pipeline_cache_all_hit = true,
    };
    if (shape.committed_submissions
        > (std::numeric_limits<std::uint64_t>::max)()
            - query.committed_submissions) {
        return {};
    }
    result.acceptance =
        PersistedShapeV5CoordinateOperationAcceptanceAudit{
            .operation_identity = proof.operation_identity_,
            .expected_job_count =
                static_cast<std::uint64_t>(proof.records_.size()),
            .consumed_count =
                static_cast<std::uint64_t>(proof.consumed_count()),
            .accepted_count =
                static_cast<std::uint64_t>(proof.accepted_count()),
            .burned_count =
                static_cast<std::uint64_t>(proof.burned_count()),
            .query_pipeline_receipt_count =
                static_cast<std::uint64_t>(query.prepared_count),
            .shape_pipeline_receipt_count =
                static_cast<std::uint64_t>(shape.prepared_count),
            .committed_submission_count = query.committed_submissions
                + shape.committed_submissions,
            .seed_receipt_identity = query.seed_receipt_identity,
        };
    if (!result.environment.accepted()
        || result.environment != environment_preview
        || !result.acceptance.accepted()
        || result.acceptance != expected_preview.acceptance) {
        return {};
    }
    result.valid = true;
    return result;
}

bool Runtime::self_test_persisted_shape_v5_finalizer_preclaim_contract(
    RuntimeMetalArchivePipelineAudit base_audit) {
    if (!base_audit.accepted() || base_audit.pipeline_create_ns == 0u) {
        return false;
    }
    const std::array<ShaderVariantKey, 11> keys{
        make_shader_key(
            ShaderKind::phase_c_sketch_tile, 384u, 64u, 10u),
        make_shader_key(
            ShaderKind::phase_c_prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(
            ShaderKind::phase_c_prefix_scan_add, 384u, 128u, 1u),
        make_shader_key(
            ShaderKind::phase_c_shape_v5_seed_count, 384u, 128u, 3u),
        make_shader_key(
            ShaderKind::phase_c_shape_v5_seed_emit, 384u, 128u, 3u),
        make_shader_key(
            ShaderKind::phase_c_chain_range_build, 384u, 1u, 1u),
        make_shader_key(
            ShaderKind::phase_c_coordinate_reduce, 384u, 128u, 1u),
        make_shader_key(
            ShaderKind::chain_tiled_dp_u64, 32u, 32u, 0u),
        make_shader_key(
            ShaderKind::phase_c_sort_radix_histogram, 384u, 1u, 1u),
        make_shader_key(
            ShaderKind::phase_c_sort_radix_scan, 384u, 1u, 1u),
        make_shader_key(
            ShaderKind::phase_c_sort_radix_scatter, 384u, 1u, 1u),
    };
    if (!std::all_of(
            keys.begin(), keys.end(), [](const ShaderVariantKey& key) {
                return valid_shader_variant_key(key);
            })) {
        return false;
    }
    std::array<RuntimeMetalArchivePipelineReceipt, 11> receipts{};
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        RuntimeMetalArchivePipelineAudit audit = base_audit;
        audit.receipt_identity = 8000u + i;
        audit.scope_identity = 8100u + i;
        audit.shader_key = keys[i];
        receipts[i] = RuntimeMetalArchivePipelineReceipt{
            std::move(audit)};
        if (!receipts[i].accepted()) return false;
    }
    const auto command = [](const std::uintptr_t identity) {
        return reinterpret_cast<VkCommandBuffer>(identity);
    };

    RuntimePipelineOperationProof query{};
    RuntimePipelineOperationProof shape{};
    try {
        auto query_collector = RuntimePipelineUseCollector::create(
            RuntimePipelineOperationKind::query_sketch, 1u, 101u);
        for (std::size_t i = 0u; i < 3u; ++i) {
            query_collector->prepare(keys[i], &receipts[i]);
        }
        query_collector->begin(command(1u));
        query_collector->use(
            command(1u), 0u, keys[0], &receipts[0], 3u);
        query_collector->use(
            command(1u), 0u, keys[1], &receipts[1], 2u);
        query_collector->use(
            command(1u), 0u, keys[2], &receipts[2], 2u);
        query_collector->commit(command(1u));
        query_collector->begin(command(2u));
        query_collector->use(
            command(2u), 0u, keys[0], &receipts[0], 3u);
        query_collector->use(
            command(2u), 0u, keys[2], &receipts[2], 1u);
        query_collector->commit(command(2u));
        query = query_collector->seal();

        auto shape_collector = RuntimePipelineUseCollector::create(
            RuntimePipelineOperationKind::shape_v5_coordinate, 1u, 202u);
        constexpr std::array<std::size_t, 7> shape_base_slots{
            3u, 4u, 1u, 2u, 5u, 6u, 7u};
        for (const std::size_t slot : shape_base_slots) {
            shape_collector->prepare(keys[slot], &receipts[slot]);
        }
        for (std::size_t slot = 8u; slot < 11u; ++slot) {
            shape_collector->prepare(keys[slot], &receipts[slot]);
        }
        shape_collector->set_shape_input(0u, 1u, 255u, 0u);
        shape_collector->set_shape_outcome(0u, 1u, 1u);

        shape_collector->begin(command(11u));
        shape_collector->use(
            command(11u), 0u, keys[3], &receipts[3], 1u);
        shape_collector->use(
            command(11u), 0u, keys[1], &receipts[1], 1u);
        shape_collector->use(
            command(11u), 0u, keys[2], &receipts[2], 1u);
        shape_collector->use(
            command(11u), 0u, keys[2], &receipts[2], 1u);
        shape_collector->commit(command(11u));

        shape_collector->begin(command(12u));
        shape_collector->use(
            command(12u), 0u, keys[4], &receipts[4], 1u);
        shape_collector->commit(command(12u));

        shape_collector->begin(command(13u));
        for (std::size_t slot = 8u; slot < 11u; ++slot) {
            shape_collector->use(
                command(13u), 0u, keys[slot], &receipts[slot], 1u);
        }
        shape_collector->use(
            command(13u), 0u, keys[5], &receipts[5], 1u);
        shape_collector->commit(command(13u));

        shape_collector->begin(command(14u));
        shape_collector->use(
            command(14u), 0u, keys[7], &receipts[7], 1u);
        shape_collector->use(
            command(14u), 0u, keys[6], &receipts[6], 1u);
        shape_collector->commit(command(14u));
        shape = shape_collector->seal();
    } catch (...) {
        return false;
    }
    if (!query.accepted_query_sketch()
        || !shape.accepted_shape_v5_coordinate()) {
        return false;
    }

    PersistedShapeV5CoordinateOperationProof proof{};
    proof.operation_identity_ = 202u;
    try {
        proof.admit_records(1u);
    } catch (...) {
        return false;
    }
    proof.records_[0].state =
        PersistedShapeV5CoordinateOperationProof::RecordState::accepted;
    proof.accepted_count_ = 1u;
    proof.query_pipeline_proof_ = std::move(query);
    proof.shape_pipeline_proof_ = std::move(shape);
    proof.sealed_ = true;
    const PersistedShapeV5CoordinateOperationEvidencePreview expected =
        proof.operation_evidence_preview();
    if (!proof.all_jobs_accepted() || !expected.accepted()
        || expected.acceptance.shape_pipeline_receipt_count != 10u) {
        return false;
    }
    VulkanFeatureSet live_features{};
    live_features.device_uuid = expected.device_uuid;
    live_features.api_version = expected.api_version;
    live_features.driver_version = expected.driver_version;
    live_features.vendor_id = expected.vendor_id;
    live_features.device_id = expected.device_id;
    live_features.pipeline_cache_uuid = expected.pipeline_cache_uuid;
    bool seed_claimed = false;
    const auto claims_unchanged = [&]() {
        return !seed_claimed
            && std::none_of(
                receipts.begin(), receipts.end(), [](const auto& receipt) {
                    return receipt.metrics_claimed_;
                });
    };
    if (!claims_unchanged()) return false;
    {
        auto accepted = prepare_persisted_shape_v5_coordinate_finalization(
            proof, expected, live_features, seed_claimed);
        if (!accepted.accepted()
            || accepted.acceptance != expected.acceptance
            || accepted.environment != proof.environment_preview()
            || accepted.cache_claim_plan.audit()
                != expected.maximum_cache_metrics
            || accepted.cache_claim_plan.committed()) {
            return false;
        }
    }
    if (!claims_unchanged()) return false;

    const auto rejected = [&](const auto& candidate,
                              const VulkanFeatureSet& live) {
        auto plan = prepare_persisted_shape_v5_coordinate_finalization(
            proof, candidate, live, seed_claimed);
        return !plan.accepted() && claims_unchanged();
    };
    const auto rejected_preview = [&](auto mutate) {
        auto candidate = expected;
        mutate(candidate);
        return rejected(candidate, live_features);
    };
    const auto rejected_live = [&](auto mutate) {
        auto candidate = live_features;
        mutate(candidate);
        return rejected(expected, candidate);
    };

    // Every fixed field crossing the SQL-preview/finalizer equality gate gets
    // an independent mutation. Rejection must leave all report-once bits clear.
    if (!rejected_preview([](auto& value) {
            ++value.acceptance.operation_identity;
            value.shape_pipeline_operation_identity =
                value.acceptance.operation_identity;
            value.maximum_cache_metrics.token_identity =
                value.acceptance.operation_identity;
        })
        || !rejected_preview([](auto& value) {
            ++value.acceptance.expected_job_count;
        })
        || !rejected_preview([](auto& value) {
            ++value.acceptance.consumed_count;
        })
        || !rejected_preview([](auto& value) {
            ++value.acceptance.accepted_count;
        })
        || !rejected_preview([](auto& value) {
            value.acceptance.burned_count = 1u;
        })
        || !rejected_preview([](auto& value) {
            ++value.acceptance.query_pipeline_receipt_count;
        })
        || !rejected_preview([](auto& value) {
            --value.acceptance.shape_pipeline_receipt_count;
        })
        || !rejected_preview([](auto& value) {
            ++value.acceptance.committed_submission_count;
        })
        || !rejected_preview([](auto& value) {
            ++value.acceptance.seed_receipt_identity;
        })
        || !rejected_preview([](auto& value) {
            ++value.query_pipeline_operation_identity;
        })
        || !rejected_preview([](auto& value) {
            ++value.shape_pipeline_operation_identity;
        })
        || !rejected_preview([](auto& value) {
            ++value.query_submission_ordinals[0];
        })
        || !rejected_preview([](auto& value) {
            ++value.query_submission_ordinals[1];
        })
        || !rejected_preview([](auto& value) {
            value.device_uuid[0] ^= 0x80u;
        })
        || !rejected_preview([](auto& value) { ++value.api_version; })
        || !rejected_preview([](auto& value) { ++value.driver_version; })
        || !rejected_preview([](auto& value) { ++value.vendor_id; })
        || !rejected_preview([](auto& value) { ++value.device_id; })
        || !rejected_preview([](auto& value) {
            value.pipeline_cache_uuid[0] ^= 0x80u;
        })
        || !rejected_preview([](auto& value) {
            value.seed_sha256[0] ^= 0x80u;
        })
        || !rejected_preview([](auto& value) { ++value.seed_size; })
        || !rejected_preview([](auto& value) {
            ++value.manifest_checksum;
        })
        || !rejected_preview([](auto& value) {
            ++value.maximum_cache_metrics.token_identity;
        })
        || !rejected_preview([](auto& value) {
            value.maximum_cache_metrics.gpu_pipeline_cache_loads = 0u;
            value.maximum_cache_metrics.gpu_pipeline_cache_seed_bytes = 0u;
            value.maximum_cache_metrics.gpu_pipeline_cache_load_ns = 0u;
        })
        || !rejected_preview([](auto& value) {
            ++value.maximum_cache_metrics.gpu_pipeline_cache_seed_bytes;
        })
        || !rejected_preview([](auto& value) {
            ++value.maximum_cache_metrics.gpu_pipeline_cache_load_ns;
        })
        || !rejected_preview([](auto& value) {
            ++value.maximum_cache_metrics.gpu_pipeline_creates;
        })
        || !rejected_preview([](auto& value) {
            ++value.maximum_cache_metrics.gpu_pipeline_creation_ns;
        })
        || !rejected_preview([](auto& value) {
            value.maximum_cache_metrics.saturated = true;
        })) {
        return false;
    }

    if (!rejected_live([](auto& value) {
            value.device_uuid[0] ^= 0x40u;
        })
        || !rejected_live([](auto& value) { ++value.api_version; })
        || !rejected_live([](auto& value) { ++value.driver_version; })
        || !rejected_live([](auto& value) { ++value.vendor_id; })
        || !rejected_live([](auto& value) { ++value.device_id; })
        || !rejected_live([](auto& value) {
            value.pipeline_cache_uuid[0] ^= 0x40u;
        })) {
        return false;
    }

    const auto rejected_receipt = [&](auto mutate) {
        RuntimeMetalArchivePipelineAudit saved = receipts[10].audit_;
        mutate(receipts[10].audit_);
        // Receipt mutations exercise the finalizer's frozen PREPARED/USED
        // equality gate, not the qualification's own self-consistency gate.
        // Keep the process qualification bound to the mutated live audit so
        // the receipt remains independently accepted before the finalizer
        // rejects its drift from the sealed operation proof.
        RuntimeMetalArchiveProcessQualification& qualification =
            receipts[10].audit_.cache_qualification;
        qualification.archive_set = receipts[10].audit_.expected_archives;
        qualification.vulkan_device_uuid =
            receipts[10].audit_.runtime_vulkan_device_uuid;
        qualification.api_version = receipts[10].audit_.api_version;
        qualification.driver_version = receipts[10].audit_.driver_version;
        qualification.vendor_id = receipts[10].audit_.vendor_id;
        qualification.device_id = receipts[10].audit_.device_id;
        qualification.pipeline_cache_uuid =
            receipts[10].audit_.pipeline_cache_uuid;
        qualification.seed_receipt_identity =
            receipts[10].audit_.seed_receipt_identity;
        qualification.seed_sha256 = receipts[10].audit_.seed_sha256;
        qualification.seed_size = receipts[10].audit_.seed_size;
        qualification.manifest_checksum =
            receipts[10].audit_.manifest_checksum;
        qualification.identity =
            runtime_metal_archive_process_qualification_identity(
                qualification);
        const bool remained_valid = receipts[10].accepted();
        const bool result = remained_valid
            && rejected(expected, live_features);
        receipts[10].audit_ = std::move(saved);
        return result && claims_unchanged();
    };
    const auto change_nonzero = [](auto& value) {
        using Value = std::remove_reference_t<decltype(value)>;
        value = value == (std::numeric_limits<Value>::max)()
            ? value - 1u
            : value + 1u;
    };
    const auto change_nonzero_byte = [](auto& bytes) {
        bytes[0] = bytes[0] == 0xffu
            ? static_cast<std::uint8_t>(0xfeu)
            : static_cast<std::uint8_t>(bytes[0] + 1u);
    };
    if (!rejected_receipt([&](auto& audit) {
            change_nonzero(audit.receipt_identity);
        })
        || !rejected_receipt([&](auto& audit) {
            audit.shader_key = keys[0];
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero_byte(audit.configured_vulkan_device_uuid);
            audit.runtime_vulkan_device_uuid =
                audit.configured_vulkan_device_uuid;
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero(audit.api_version);
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero(audit.driver_version);
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero(audit.vendor_id);
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero(audit.device_id);
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero_byte(audit.pipeline_cache_uuid);
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero_byte(audit.seed_sha256);
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero(audit.seed_size);
            audit.seed_artifact_bytes = audit.seed_size;
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero(audit.manifest_checksum);
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero(audit.seed_receipt_identity);
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero(audit.expected_archives.ordered_checksum);
            audit.loaded_archives = audit.expected_archives;
        })
        || !rejected_receipt([&](auto& audit) {
            change_nonzero(audit.pipeline_create_ns);
        })) {
        return false;
    }
    return claims_unchanged();
}

bool Runtime::self_test_gpu_cache_partial_delta_contract(
    RuntimeMetalArchivePipelineAudit base_audit) {
    if (!base_audit.accepted() || base_audit.pipeline_create_ns == 0u) {
        return false;
    }
    const std::array<ShaderVariantKey, 3> keys{
        make_shader_key(
            ShaderKind::phase_c_sketch_tile, 384u, 64u, 10u),
        make_shader_key(
            ShaderKind::phase_c_prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(
            ShaderKind::phase_c_prefix_scan_add, 384u, 128u, 1u),
    };
    if (!std::all_of(
            keys.begin(), keys.end(), [](const ShaderVariantKey& key) {
                return valid_shader_variant_key(key);
            })) {
        return false;
    }
    std::array<RuntimeMetalArchivePipelineReceipt, 3> receipts{};
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        RuntimeMetalArchivePipelineAudit audit = base_audit;
        audit.receipt_identity = 7000u + i;
        audit.scope_identity = 7100u + i;
        audit.shader_key = keys[i];
        receipts[i] = RuntimeMetalArchivePipelineReceipt{
            std::move(audit)};
        if (!receipts[i].accepted()) return false;
    }
    std::array<const RuntimeMetalArchivePipelineReceipt*, 3> pointers{};
    RuntimeGpuCacheMetricsAudit maximum{
        .token_identity = 7200u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = receipts[0].audit().seed_size,
        .gpu_pipeline_cache_load_ns = receipts[0].audit().seed_load_ns,
        .gpu_pipeline_creates =
            static_cast<std::uint64_t>(receipts.size()),
    };
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        pointers[i] = &receipts[i];
        if (receipts[i].audit().pipeline_create_ns
            > (std::numeric_limits<std::uint64_t>::max)()
                - maximum.gpu_pipeline_creation_ns) {
            return false;
        }
        maximum.gpu_pipeline_creation_ns +=
            receipts[i].audit().pipeline_create_ns;
    }
    if (!maximum.accepted()) return false;

    bool seed_claimed = false;
    RuntimeGpuCacheMetricsAudit subset_maximum{
        .token_identity = 7199u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = receipts[0].audit().seed_size,
        .gpu_pipeline_cache_load_ns = receipts[0].audit().seed_load_ns,
        .gpu_pipeline_creates = 1u,
        .gpu_pipeline_creation_ns =
            receipts[0].audit().pipeline_create_ns,
    };
    auto subset = runtime_detail::RuntimeGpuCacheMetricsClaimPlan::prepare(
        subset_maximum.token_identity, subset_maximum, seed_claimed,
        std::span<const RuntimeMetalArchivePipelineReceipt* const>{
            pointers.data(), 1u});
    if (!subset.accepted() || subset.audit() != subset_maximum) {
        return false;
    }
    subset.commit(seed_claimed);
    if (!subset.committed() || !seed_claimed
        || !receipts[0].metrics_claimed_
        || receipts[1].metrics_claimed_
        || receipts[2].metrics_claimed_) {
        return false;
    }

    auto remainder = runtime_detail::RuntimeGpuCacheMetricsClaimPlan::prepare(
        maximum.token_identity, maximum, seed_claimed,
        std::span<const RuntimeMetalArchivePipelineReceipt* const>{
            pointers.data(), pointers.size()});
    if (!remainder.accepted()) return false;
    const RuntimeGpuCacheMetricsAudit partial = remainder.audit();
    if (!partial.componentwise_bounded_by(maximum)
        || partial == maximum
        || partial.gpu_pipeline_cache_loads != 0u
        || partial.gpu_pipeline_cache_seed_bytes != 0u
        || partial.gpu_pipeline_cache_load_ns != 0u
        || partial.gpu_pipeline_creates != 2u
        || partial.gpu_pipeline_creation_ns == 0u) {
        return false;
    }
    remainder.commit(seed_claimed);
    if (!remainder.committed()
        || std::any_of(
            receipts.begin(), receipts.end(), [](const auto& receipt) {
                return !receipt.metrics_claimed_;
            })) {
        return false;
    }

    RuntimeGpuCacheMetricsDelta issued{};
    issued.audit_ = partial;
    issued.issued_ = true;
    RuntimeGpuCacheMetricsDelta moved{std::move(issued)};
    if (issued.accepted() || issued.audit() != RuntimeGpuCacheMetricsAudit{}
        || !moved.accepted() || moved.audit() != partial) {
        return false;
    }
    moved.consume();
    if (moved.accepted()
        || moved.audit() != RuntimeGpuCacheMetricsAudit{}) {
        return false;
    }

    RuntimeGpuCacheMetricsAudit later_maximum = maximum;
    later_maximum.token_identity = 7201u;
    auto later = runtime_detail::RuntimeGpuCacheMetricsClaimPlan::prepare(
        later_maximum.token_identity, later_maximum, seed_claimed,
        std::span<const RuntimeMetalArchivePipelineReceipt* const>{
            pointers.data(), pointers.size()});
    const RuntimeGpuCacheMetricsAudit zero = later.audit();
    if (!later.accepted()
        || zero.token_identity != later_maximum.token_identity
        || zero.gpu_pipeline_cache_loads != 0u
        || zero.gpu_pipeline_cache_seed_bytes != 0u
        || zero.gpu_pipeline_cache_load_ns != 0u
        || zero.gpu_pipeline_creates != 0u
        || zero.gpu_pipeline_creation_ns != 0u
        || zero.saturated) {
        return false;
    }
    later.commit(seed_claimed);
    if (!later.committed()) return false;
    RuntimeGpuCacheMetricsDelta zero_delta{};
    zero_delta.audit_ = zero;
    zero_delta.issued_ = true;
    if (!zero_delta.accepted()) return false;
    zero_delta.consume();
    return !zero_delta.accepted()
        && zero_delta.audit() == RuntimeGpuCacheMetricsAudit{};
}

PersistedShapeV5CoordinateAcceptedOperation
Runtime::finalize_persisted_shape_v5_coordinate_operation(
    PersistedShapeV5CoordinateOperationProof&& proof,
    const PersistedShapeV5CoordinateOperationEvidencePreview&
        expected_preview) {
    std::scoped_lock lock(mu_);
    auto plan = prepare_persisted_shape_v5_coordinate_finalization(
        proof, expected_preview, features_, seed_metrics_claimed_);
    if (!plan.accepted()) {
        throw VulkanRuntimeError(
            "persisted Shape-V5 operation finalization payload is invalid");
    }

    // This is deliberately the last external-state check. Everything after it
    // is fixed-field move/copy construction and cannot dispatch or allocate.
    require_immutable_metal_policy_live(
        "persisted Shape-V5 operation final publication");

    // No failing work follows this mutation. The queue/cache mutex makes the
    // seed and receipt claims one transaction with authority minting.
    const RuntimeGpuCacheMetricsAudit cache_metrics =
        plan.cache_claim_plan.audit();
    plan.cache_claim_plan.commit(seed_metrics_claimed_);
    PersistedShapeV5CoordinateAcceptedOperation accepted{};
    accepted.audit_ = plan.acceptance;
    accepted.environment_ = plan.environment;
    accepted.cache_metrics_.audit_ = cache_metrics;
    accepted.cache_metrics_.issued_ = true;
    accepted.issued_ = true;
    proof = PersistedShapeV5CoordinateOperationProof{};
    return accepted;
}

std::vector<GpuCoordinateOnlyResultWire>
Runtime::run_phase_c_seed_to_coordinate_batch_roundtrip(
    const PhaseCSeedQueryInputs& query_inputs,
    std::span<const PhaseCSeedCoordinateJob> jobs,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    PhaseCSeedCoordinateAlgorithm coordinate_algorithm,
    const ChainingPipelineKey& chain_key,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit)
{
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate query length exceeds ABI");
    }
    const PhaseCSeedQueryRecordWire qrec{
        .minimizer_offset = 0u,
        .minimizer_count = query_inputs.minimizer_count,
        .query_len = static_cast<std::uint32_t>(query_len),
        .flags = 0u,
    };
    PhaseCSeedQueryInputs forwarded = query_inputs;
    forwarded.host_query_records =
        std::span<const PhaseCSeedQueryRecordWire>{&qrec, 1u};
    forwarded.device_query_records = nullptr;
    return run_phase_c_seed_to_coordinate_multi_query_batch_roundtrip(
        forwarded,
        jobs,
        kmer_size,
        max_occ,
        coordinate_algorithm,
        chain_key,
        max_dist_x,
        max_dist_y,
        bandwidth,
        max_iter,
        gap_penalty,
        skip_penalty,
        max_chain_skip,
        min_chain_score,
        min_seed_count,
        probe_limit).results;
}

GpuCoordinateOnlyResultWire Runtime::run_phase_c_seed_to_coordinate_roundtrip(
    const PhaseCSeedInputs& inputs,
    std::uint64_t query_len,
    std::uint64_t target_len,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    const ChainingPipelineKey& chain_key,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit)
{
    std::lock_guard<std::mutex> guard(mu_);
    GpuCoordinateOnlyResultWire result{};
    if (!features_.available) {
        throw VulkanRuntimeError("run_phase_c_seed_to_coordinate_roundtrip on unavailable device");
    }
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate missing resident index buffers");
    }
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate query length exceeds ABI");
    }
    const std::uint32_t mc = inputs.minimizer_count;
    if (mc == 0u || max_occ < 0) return result;

    struct ChainRangePushLocal {
        std::uint32_t query_count;
        std::uint32_t max_ref_gap;
    };
    static_assert(sizeof(ChainRangePushLocal) == 8);

    GpuArena& arena = phase_c_arena();
    const std::uint32_t num_blocks = ceil_div_u32(mc, kPhaseCScanBlock);
    const std::uint64_t min_words_bytes = inputs.device_minimizer_words == nullptr
        ? std::max<std::uint64_t>(
              minimizer_device_bytes(
                  inputs.minimizer_word_kind, inputs.minimizer_words32_per_record, mc),
              4u)
        : 0u;
    std::uint64_t dt = std::uint64_t{mc} * 4u * 2u + std::uint64_t{num_blocks} * 4u
        + 16u + min_words_bytes + 6u * std::uint64_t{kPhaseCSlotOffsetAlign};
    if (!arena.ensure_capacity(dt, 0, 0)) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate arena grow failed");
    }
    const GpuArenaMark mark = arena.mark();

    const GpuArenaSlice counts_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const GpuArenaSlice prefix_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const GpuArenaSlice blocks_s = arena.allocate_device(
        std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const GpuArenaSlice qrec_s = arena.allocate_device(16u, 16u);
    GpuArenaSlice minw_s{};
    if (inputs.device_minimizer_words == nullptr) {
        minw_s = arena.allocate_device(min_words_bytes, 16u);
    }
    if (!counts_s.valid() || !prefix_s.valid() || !blocks_s.valid() || !qrec_s.valid()
        || (inputs.device_minimizer_words == nullptr && !minw_s.valid())) {
        arena.rewind(mark);
        throw VulkanRuntimeError("phase_c seed-to-coordinate arena oversubscribed");
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    const VkBuffer min_words_buffer = inputs.device_minimizer_words != nullptr
        ? inputs.device_minimizer_words->buffer
        : minw_s.buffer->buffer;
    const std::uint64_t min_words_offset =
        inputs.device_minimizer_words != nullptr ? 0u : minw_s.offset;
    const std::uint64_t min_words_range =
        inputs.device_minimizer_words != nullptr ? VK_WHOLE_SIZE : minw_s.size;
    if (inputs.device_minimizer_words == nullptr) {
        std::memset(base + minw_s.offset, 0, static_cast<std::size_t>(minw_s.size));
        if (!inputs.host_minimizer_bytes.empty()) {
            std::memcpy(base + minw_s.offset,
                        inputs.host_minimizer_bytes.data(),
                        inputs.host_minimizer_bytes.size());
        }
    }
    const HostQRec qrec{0u, mc, static_cast<std::uint32_t>(query_len), 0u};
    std::memcpy(base + qrec_s.offset, &qrec, sizeof(qrec));

    const PhaseBPipeline count_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_count, 128u, 1u);
    const PhaseBPipeline emit_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_emit, 128u, 1u);
    const PhaseBPipeline scanb =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scana =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);
    const PhaseBPipeline range_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_chain_range_build, 1u, 1u);
    const PhaseBPipeline coord_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_coordinate_reduce, 128u, 1u);
    const VkPipeline chain_pipeline =
        create_pipeline_locked(chain_key).pipeline;

    const std::uint32_t target_offset_lo =
        static_cast<std::uint32_t>(inputs.target_position_offset & 0xffffffffull);
    const std::uint32_t target_offset_hi =
        static_cast<std::uint32_t>(inputs.target_position_offset >> 32);
    const PhaseCAnchorSortKeyPlan anchor_sort_plan =
        phase_c_anchor_sort_key_plan(
            inputs.target_len != 0u ? inputs.target_len : target_len,
            inputs.target_position_offset);
    SeedPushC cpush{inputs.minimizer_words32_per_record, inputs.minimizer_word_kind,
        inputs.hash_shift, inputs.pos_bits, kmer_size, inputs.slot_mask,
        static_cast<std::uint32_t>(max_occ), 0u, 1u, inputs.bucket_count,
        target_offset_lo, target_offset_hi, anchor_sort_plan.target_ref_bits};
    const std::uint32_t seed_groups = std::max<std::uint32_t>(
        1u,
        std::min(ceil_div_u32(mc, 128u), features_.max_workgroup_count_x));

    {
        reset_phase_c_pool_locked();
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        }};
        const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {blocks_s.buffer->buffer, blocks_s.offset, blocks_s.size},
        }};
        ScanPushC s0{mc, kPhaseCScanBlock, num_blocks, 0u};
        ScanPushC s1{mc, kPhaseCScanBlock, num_blocks, 1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord1)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord1)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, count_pipe, seed_infos, &cpush, sizeof(cpush), seed_groups);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, scanb, scan_infos, &s0, sizeof(s0), num_blocks);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, scana, scan_infos, &s0, sizeof(s0), 1u);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, scana, scan_infos, &s1, sizeof(s1), ceil_div_u32(mc, 128u));
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord1)");
    }

    std::uint32_t plast = 0, clast = 0;
    std::memcpy(&plast, base + prefix_s.offset + (mc - 1u) * 4u, 4);
    std::memcpy(&clast, base + counts_s.offset + (mc - 1u) * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(plast) + clast;
    if (total == 0u) {
        arena.rewind(mark);
        return result;
    }
    if (total > std::numeric_limits<std::uint32_t>::max()) {
        arena.rewind(mark);
        throw VulkanRuntimeError("phase_c seed-to-coordinate anchor count overflow");
    }
    const auto total_u32 = static_cast<std::uint32_t>(total);

    const std::uint64_t anchor_u64 = total * 3u;
    const WgPlan awg = plan_workgroups(total, features_.max_workgroup_count_x);
    Buffer anchors_buf = create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
    Buffer akeys_buf = create_device_buffer(total * 8u, kPhaseBBufferUsage);
    Buffer anchors_scr_buf = create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
    Buffer akeys_scr_buf = create_device_buffer(total * 8u, kPhaseBBufferUsage);
    Buffer ahist_buf = create_device_buffer(
        std::uint64_t{awg.num_workgroups} * 256u * 4u, kPhaseBBufferUsage);
    Buffer ranges_buf = create_device_buffer(
        std::max<std::uint64_t>(total * sizeof(GpuChainRangeWire), 8u),
        kPhaseBBufferUsage);
    Buffer range_count_buf = create_device_buffer(8u, kPhaseBBufferUsage);
    Buffer compacted_anchors_buf = create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
    Buffer span_buf = create_device_buffer(sizeof(GpuChainRangeBuildSpanWire), kPhaseBBufferUsage);
    if (anchors_buf.mapped == nullptr || ranges_buf.mapped == nullptr
        || range_count_buf.mapped == nullptr || compacted_anchors_buf.mapped == nullptr
        || span_buf.mapped == nullptr) {
        arena.rewind(mark);
        throw VulkanRuntimeError("phase_c seed-to-coordinate non-unified memory");
    }
    const GpuArenaSlice anchors_s{&anchors_buf, 0, anchors_buf.bytes};
    const GpuArenaSlice akeys_s{&akeys_buf, 0, akeys_buf.bytes};
    const GpuArenaSlice anchors_scr_s{&anchors_scr_buf, 0, anchors_scr_buf.bytes};
    const GpuArenaSlice akeys_scr_s{&akeys_scr_buf, 0, akeys_scr_buf.bytes};
    const GpuArenaSlice ahist_s{&ahist_buf, 0, ahist_buf.bytes};
    const GpuArenaSlice compacted_anchors_s{
        &compacted_anchors_buf, 0, compacted_anchors_buf.bytes};

    const GpuChainRangeBuildSpanWire span{
        .anchor_lo = 0u,
        .anchor_hi = total_u32,
        .range_lo = 0u,
        .range_out = 0u,
    };
    std::memcpy(span_buf.mapped, &span, sizeof(span));
    std::memset(range_count_buf.mapped, 0, 8u);

    bool sorted_in_scratch = false;
    {
        reset_phase_c_pool_locked();
        SeedPushC epush = cpush;
        epush.emit_mode = 1u;
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {anchors_s.buffer->buffer, anchors_s.offset, anchors_s.size},
            {akeys_s.buffer->buffer, akeys_s.offset, akeys_s.size},
        }};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord2)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord2)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, emit_pipe, seed_infos, &epush, sizeof(epush), seed_groups);
        record_compute_barrier(command_buffer_);
        GpuSortPlan sort_plan{total, 8u, 24u, 8u, anchor_sort_plan.pass_count};
        record_radix_sort_locked(
            command_buffer_, akeys_s, anchors_s, akeys_scr_s, anchors_scr_s, ahist_s,
            sort_plan, awg.num_workgroups, awg.elems_per_wg, 3u, sorted_in_scratch);
        const GpuArenaSlice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
        const std::array<VkDescriptorBufferInfo, 6> range_infos{{
            {sorted_anchors.buffer->buffer, sorted_anchors.offset, sorted_anchors.size},
            {range_count_buf.buffer, 0, range_count_buf.bytes},
            {ranges_buf.buffer, 0, ranges_buf.bytes},
            {range_count_buf.buffer, 0, range_count_buf.bytes},
            {compacted_anchors_s.buffer->buffer, compacted_anchors_s.offset,
             compacted_anchors_s.size},
            {span_buf.buffer, 0, span_buf.bytes},
        }};
        ChainRangePushLocal rpush{
            .query_count = 1u,
            .max_ref_gap = static_cast<std::uint32_t>(std::max<std::int32_t>(max_dist_x, 0)),
        };
        record_phase_c_dispatch_locked(
            command_buffer_, range_pipe, range_infos, &rpush, sizeof(rpush), 1u);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord2)");
    }

    std::uint32_t range_count = 0;
    std::uint32_t compacted_anchor_count = 0;
    std::memcpy(&range_count, range_count_buf.mapped, 4u);
    std::memcpy(&compacted_anchor_count, static_cast<std::byte*>(range_count_buf.mapped) + 4u, 4u);
    if (range_count == 0u) {
        arena.rewind(mark);
        return result;
    }
    if (compacted_anchor_count > total_u32) {
        arena.rewind(mark);
        throw VulkanRuntimeError("phase_c seed-to-coordinate compacted anchor overflow");
    }
    const bool use_compacted_anchors =
        compacted_anchor_count != 0u && compacted_anchor_count < total_u32;
    const std::uint32_t active_anchor_count =
        use_compacted_anchors ? compacted_anchor_count : total_u32;

    Buffer score_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::int32_t), kPhaseBBufferUsage);
    Buffer pred_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::int64_t), kPhaseBBufferUsage);
    Buffer visited_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::uint32_t), kPhaseBBufferUsage);
    Buffer trace_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::uint32_t), kPhaseBBufferUsage);
    Buffer result_buf = create_device_buffer(sizeof(GpuCoordinateOnlyResultWire), kPhaseBBufferUsage);
    const GpuArenaSlice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
    const GpuArenaSlice& active_anchors =
        use_compacted_anchors ? compacted_anchors_s : sorted_anchors;

    {
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord3)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord3)");

        std::memset(score_buf.mapped, 0, static_cast<std::size_t>(score_buf.bytes));
        std::memset(pred_buf.mapped, 0xff, static_cast<std::size_t>(pred_buf.bytes));
        std::memset(visited_buf.mapped, 0, static_cast<std::size_t>(visited_buf.bytes));
        std::memset(result_buf.mapped, 0, static_cast<std::size_t>(result_buf.bytes));
        record_host_to_shader(command_buffer_);

        const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
            {active_anchors.buffer->buffer, active_anchors.offset, active_anchors.size},
            {ranges_buf.buffer, 0, ranges_buf.bytes},
            {score_buf.buffer, 0, score_buf.bytes},
            {pred_buf.buffer, 0, pred_buf.bytes},
        }};
        VkDescriptorSet chain_set = alloc_phase_c_set_locked(descriptor_set_layout_);
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (std::uint32_t b = 0; b < 4; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = chain_set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &chain_infos[b];
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        ChainingPushConstants chain_push{};
        chain_push.anchor_count = active_anchor_count;
        chain_push.range_count = range_count;
        chain_push.max_dist_x = max_dist_x;
        chain_push.max_dist_y = max_dist_y;
        chain_push.bandwidth = bandwidth;
        chain_push.max_iter = max_iter;
        chain_push.gap_penalty = gap_penalty;
        chain_push.skip_penalty = skip_penalty;
        chain_push.max_chain_skip = max_chain_skip;
        const std::uint32_t chain_groups =
            std::min(range_count, features_.max_workgroup_count_x);

        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, chain_pipeline);
        vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline_layout_, 0, 1, &chain_set, 0, nullptr);
        vkCmdPushConstants(command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(chain_push), &chain_push);
        vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
        record_compute_barrier(command_buffer_);

        const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
            {active_anchors.buffer->buffer, active_anchors.offset, active_anchors.size},
            {score_buf.buffer, 0, score_buf.bytes},
            {pred_buf.buffer, 0, pred_buf.bytes},
            {visited_buf.buffer, 0, visited_buf.bytes},
            {trace_buf.buffer, 0, trace_buf.bytes},
            {result_buf.buffer, 0, result_buf.bytes},
        }};
        GpuCoordinateReducePush coord_push{
            .anchor_count = active_anchor_count,
            .query_len_lo = static_cast<std::uint32_t>(query_len & 0xffffffffull),
            .query_len_hi = static_cast<std::uint32_t>(query_len >> 32),
            .min_chain_score = min_chain_score,
            .min_seed_count = min_seed_count,
            .probe_limit = probe_limit,
            .reserved = 0u,
        };
        record_phase_c_dispatch_locked(
            command_buffer_, coord_pipe, coord_infos,
            &coord_push, sizeof(coord_push), 1u);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord3)");
    }

    std::memcpy(&result, result_buf.mapped, sizeof(result));
    arena.rewind(mark);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

GpuPersistedDirectorySelectionResult
Runtime::run_persisted_directory_ordinal_selection(
    const GpuPersistedDirectorySelectionInputs& inputs)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError(
            "persisted directory selection requested on an unavailable Vulkan device");
    }
    if (inputs.minimizer_word_kind
            > static_cast<std::uint32_t>(MinimizerWordKind::u64x2)
        || (inputs.minimizer_word_kind
                == static_cast<std::uint32_t>(MinimizerWordKind::u16)
            ? inputs.minimizer_words32_per_record != 0u
            : inputs.minimizer_words32_per_record
                != (inputs.minimizer_word_kind
                        == static_cast<std::uint32_t>(MinimizerWordKind::u32)
                    ? 1u
                    : inputs.minimizer_word_kind
                            == static_cast<std::uint32_t>(MinimizerWordKind::u64)
                        ? 2u : 4u))) {
        throw VulkanRuntimeError(
            "persisted directory selection has an unsupported minimizer wire");
    }
    if (inputs.directory_entry_count
            > (std::numeric_limits<std::uint32_t>::max)()
                / k_gpu_persisted_directory_entry_words
        || (inputs.minimizer_word_kind
                != static_cast<std::uint32_t>(MinimizerWordKind::u16)
            && inputs.minimizer_count
                > (std::numeric_limits<std::uint32_t>::max)()
                    / inputs.minimizer_words32_per_record)) {
        throw VulkanRuntimeError(
            "persisted directory selection exceeds 32-bit shader indexing");
    }
    if (inputs.minimizer_count == 0u || inputs.directory_entry_count == 0u) {
        return GpuPersistedDirectorySelectionResult{
            .ordinals = {},
            // No dispatch ran, so no query work may be attested even when a
            // nonempty query was presented to an empty authoritative directory.
            .device_query_minimizers_processed = 0u,
        };
    }
    if (inputs.device_minimizer_words == nullptr
        || !inputs.device_minimizer_words->valid()
        || inputs.encoded_directory == nullptr
        || !inputs.encoded_directory->valid()) {
        throw VulkanRuntimeError(
            "persisted directory selection requires resident device buffers");
    }

    const std::uint64_t query_bytes = minimizer_device_bytes(
        inputs.minimizer_word_kind,
        inputs.minimizer_words32_per_record,
        inputs.minimizer_count);
    const std::uint64_t directory_bytes =
        static_cast<std::uint64_t>(inputs.directory_entry_count)
        * k_gpu_persisted_directory_entry_bytes;
    if (query_bytes > inputs.device_minimizer_words->bytes
        || directory_bytes > inputs.encoded_directory->bytes) {
        throw VulkanRuntimeError(
            "persisted directory selection input buffer is truncated");
    }

    const std::uint64_t bitset_word_count64 =
        (static_cast<std::uint64_t>(inputs.directory_entry_count) + 31u) / 32u;
    const std::uint64_t bitset_bytes =
        bitset_word_count64 * sizeof(std::uint32_t);
    const std::uint64_t ordinal_bytes =
        (static_cast<std::uint64_t>(inputs.directory_entry_count) + 1u)
        * sizeof(std::uint32_t);
    const std::uint64_t transient_bytes = sat_add(bitset_bytes, ordinal_bytes);
    auto transient_permit =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(transient_bytes);
    if (!transient_permit) {
        throw VulkanRuntimeError(fmt::format(
            "persisted directory selection memory admission failed for {} bytes",
            transient_bytes));
    }

    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    // `transient_permit` is declared before both buffers, so it is destroyed
    // after them on every success/error exit.
    Buffer selected_bits = create_device_buffer(bitset_bytes, usage);
    Buffer selected_ordinals = create_device_buffer(ordinal_bytes, usage);
    if (selected_bits.mapped == nullptr || selected_ordinals.mapped == nullptr) {
        throw VulkanRuntimeError(
            "persisted directory selection requires host-visible ordinal buffers");
    }
    std::memset(selected_bits.mapped, 0, static_cast<std::size_t>(bitset_bytes));
    std::memset(selected_ordinals.mapped, 0, static_cast<std::size_t>(ordinal_bytes));

    const PhaseBPipeline pipeline = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_persisted_directory_select, 128u, 1u);
    const std::array<VkDescriptorBufferInfo, 4> infos{{
        {inputs.device_minimizer_words->buffer, 0, query_bytes},
        {inputs.encoded_directory->buffer, 0, directory_bytes},
        {selected_bits.buffer, 0, bitset_bytes},
        {selected_ordinals.buffer, 0, ordinal_bytes},
    }};
    const GpuPersistedDirectorySelectionPush push{
        .minimizer_count = inputs.minimizer_count,
        .minimizer_word_kind = inputs.minimizer_word_kind,
        .minimizer_words32_per_record = inputs.minimizer_words32_per_record,
        .hash_shift = inputs.hash_shift,
        .directory_entry_count = inputs.directory_entry_count,
        .reserved_directory = 0u,
        .bitset_word_count = static_cast<std::uint32_t>(bitset_word_count64),
        .reserved = 0u,
    };

    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0),
             "reset(persisted_directory_select)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
             "begin(persisted_directory_select)");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask =
        VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    host_to_shader.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
        1, &host_to_shader, 0, nullptr, 0, nullptr);
    record_phase_c_dispatch_locked(
        command_buffer_, pipeline, infos, &push, sizeof(push), 1u);

    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(
        command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0,
        1, &shader_to_host, 0, nullptr, 0, nullptr);
    check_vk(vkEndCommandBuffer(command_buffer_),
             "end(persisted_directory_select)");
    submit_and_wait(
        device_, queue_, command_buffer_, fence_,
        "submit(persisted_directory_select)");

    const auto* ordinal_words =
        static_cast<const std::uint32_t*>(selected_ordinals.mapped);
    const std::uint32_t selected_count = ordinal_words[0];
    if (selected_count > inputs.directory_entry_count) {
        throw VulkanRuntimeError(
            "persisted directory selector returned an invalid count");
    }
    GpuPersistedDirectorySelectionResult result{
        .ordinals = std::vector<std::uint32_t>(
            ordinal_words + 1u, ordinal_words + 1u + selected_count),
        .device_query_minimizers_processed = inputs.minimizer_count,
    };
    for (std::size_t index = 0; index < result.ordinals.size(); ++index) {
        if (result.ordinals[index] >= inputs.directory_entry_count
            || (index > 0
                && result.ordinals[index] <= result.ordinals[index - 1u])) {
            throw VulkanRuntimeError(
                "persisted directory selector returned malformed ordinals");
        }
    }
    check_vk(vkResetCommandBuffer(command_buffer_, 0),
             "reset(persisted_directory_select_post)");
    return result;
}

GpuGrouped32RuntimeStreamState Runtime::begin_grouped32_runtime_stream(
    const GpuGrouped32RuntimeStateInputs& inputs)
{
    require_hardware_device(*this, "grouped32 persisted stream");
    std::lock_guard<std::mutex> guard(mu_);
    const bool valid_query_buffers = inputs.device_query_records != nullptr
        && inputs.device_query_records->valid()
        && inputs.device_query_minimizers != nullptr
        && inputs.device_query_minimizers->valid();
    const std::uint64_t query_record_bytes =
        static_cast<std::uint64_t>(inputs.query_record_count)
        * sizeof(GpuGrouped32QueryRecordWire);
    const std::uint64_t query_minimizer_bytes =
        static_cast<std::uint64_t>(inputs.query_minimizer_count)
        * sizeof(GpuGrouped32QueryMinimizerWire);
    if (!valid_query_buffers || inputs.query_identity == 0u
        || inputs.query_record_count == 0u
        || inputs.query_minimizer_count == 0u
        || inputs.occurrence_count != inputs.query_minimizer_count
        || inputs.match_capacity == 0u || inputs.directory_entry_count == 0u
        || inputs.max_occ == 0u || inputs.kmer_size != 15u
        || query_record_bytes > inputs.device_query_records->bytes
        || query_minimizer_bytes > inputs.device_query_minimizers->bytes) {
        throw VulkanRuntimeError(
            "grouped32 persisted stream requires bound unique device query state");
    }
    const std::uint64_t occurrence_bytes =
        static_cast<std::uint64_t>(inputs.occurrence_count)
        * sizeof(GpuGrouped32OccurrenceStateWire);
    const std::uint64_t match_bytes =
        static_cast<std::uint64_t>(inputs.match_capacity)
        * sizeof(GpuGrouped32StreamMatchWire);
    const std::uint64_t authorization_bytes =
        static_cast<std::uint64_t>(inputs.directory_entry_count)
        * sizeof(std::uint32_t);
    if (occurrence_bytes > features_.max_storage_buffer_range
        || match_bytes > features_.max_storage_buffer_range
        || authorization_bytes > features_.max_storage_buffer_range) {
        throw VulkanRuntimeError(
            "grouped32 persisted stream state exceeds device limits");
    }
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const std::array<std::uint64_t, 4> requested_buffer_bytes{{
        occurrence_bytes,
        match_bytes,
        sizeof(GpuGrouped32StreamStateWire),
        authorization_bytes,
    }};
    std::array<std::uint64_t, 4> admitted_buffer_bytes{};
    std::uint64_t admitted_bytes = 0u;
    for (std::size_t index = 0u; index < requested_buffer_bytes.size();
         ++index) {
        admitted_buffer_bytes[index] = buffer_memory_requirement_locked(
            requested_buffer_bytes[index], usage);
        admitted_bytes = sat_add(
            admitted_bytes, admitted_buffer_bytes[index]);
    }
    if (admitted_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        throw VulkanRuntimeError(
            "grouped32 persisted stream physical allocation overflow");
    }
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(admitted_bytes);
    if (!permit) {
        throw VulkanRuntimeError(fmt::format(
            "grouped32 persisted stream memory admission failed for {} bytes",
            admitted_bytes));
    }
    GpuGrouped32RuntimeStreamState state;
    state.permit_ = std::move(*permit);
    state.occurrences_ = create_device_buffer(occurrence_bytes, usage);
    state.matches_ = create_device_buffer(match_bytes, usage);
    state.stream_state_ = create_device_buffer(
        sizeof(GpuGrouped32StreamStateWire), usage);
    state.authorization_flags_ = create_device_buffer(
        authorization_bytes, usage);
    const std::array<const Buffer*, 4> owned{{
        &state.occurrences_, &state.matches_, &state.stream_state_,
        &state.authorization_flags_}};
    std::uint64_t allocation_bytes = 0u;
    for (std::size_t index = 0u; index < owned.size(); ++index) {
        const Buffer* buffer = owned[index];
        if (buffer->mapped == nullptr || !buffer->host_visible_coherent()) {
            throw VulkanRuntimeError(
                "grouped32 persisted stream requires coherent mapped state");
        }
        if (buffer->allocation_bytes > admitted_buffer_bytes[index]) {
            throw VulkanRuntimeError(
                "grouped32 persisted stream allocation exceeded preflight");
        }
        allocation_bytes = sat_add(allocation_bytes, buffer->allocation_bytes);
        std::memset(buffer->mapped, 0,
            static_cast<std::size_t>(buffer->bytes));
    }
    if (allocation_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || allocation_bytes > state.permit_.bytes()) {
        throw VulkanRuntimeError(
            "grouped32 persisted stream physical allocation admission failed");
    }
    state.permit_.shrink_to(allocation_bytes);
    state.query_records_ = inputs.device_query_records;
    state.query_minimizers_ = inputs.device_query_minimizers;
    state.query_record_count_ = inputs.query_record_count;
    state.query_minimizer_count_ = inputs.query_minimizer_count;
    state.occurrence_count_ = inputs.occurrence_count;
    state.match_capacity_ = inputs.match_capacity;
    state.directory_entry_count_ = inputs.directory_entry_count;
    state.expected_control_groups_ = inputs.expected_control_groups;
    state.control_groups_dispatched_.assign(
        inputs.expected_control_groups, false);
    state.max_occ_ = inputs.max_occ;
    state.kmer_size_ = inputs.kmer_size;
    state.query_identity_ = inputs.query_identity;
    state.control_state_identity_ =
        grouped32_stream_identity_.fetch_add(1u, std::memory_order_relaxed);
    if (state.control_state_identity_ == 0u) {
        state.control_state_identity_ =
            grouped32_stream_identity_.fetch_add(1u, std::memory_order_relaxed);
    }
    return state;
}

GpuGrouped32RuntimeControlReceipt
Runtime::run_grouped32_control_validate_count(
    GpuGrouped32RuntimeStreamState& state,
    const GpuGrouped32RawGroupInputs& inputs)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available || inputs.receipt_identity == 0u
        || inputs.descriptors == nullptr || !inputs.descriptors->valid()
        || inputs.payload == nullptr || !inputs.payload->valid()
        || inputs.descriptor_count == 0u
        || inputs.payload_element_count == 0u
        || inputs.group_ordinal >= state.expected_control_groups_
        || state.control_groups_dispatched_[inputs.group_ordinal]
        || std::find(state.consumed_receipt_identities_.begin(),
                state.consumed_receipt_identities_.end(),
                inputs.receipt_identity)
            != state.consumed_receipt_identities_.end()
        || static_cast<std::uint64_t>(inputs.descriptor_count)
                * sizeof(GpuGrouped32PartitionWire)
            > inputs.descriptors->bytes
        || static_cast<std::uint64_t>(inputs.payload_element_count)
                * sizeof(GpuGrouped32ControlWire)
            > inputs.payload->bytes) {
        throw VulkanRuntimeError("invalid grouped32 raw control group");
    }
    auto* stream = static_cast<GpuGrouped32StreamStateWire*>(
        state.stream_state_.mapped);
    if (stream->status != 0u || stream->completion != 0u
        || stream->control_groups_completed >= state.expected_control_groups_) {
        throw VulkanRuntimeError("grouped32 control state is not dispatchable");
    }
    state.control_groups_dispatched_[inputs.group_ordinal] = true;
    state.consumed_receipt_identities_.push_back(inputs.receipt_identity);
    const std::uint32_t before_upper = stream->matched_anchor_upper_bound;
    const PhaseBPipeline pipeline = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_grouped32_stream_control, 128u, 2u);
    const std::array<VkDescriptorBufferInfo, 9> infos{{
        {inputs.descriptors->buffer, 0,
            static_cast<VkDeviceSize>(inputs.descriptor_count)
                * sizeof(GpuGrouped32PartitionWire)},
        {inputs.payload->buffer, 0,
            static_cast<VkDeviceSize>(inputs.payload_element_count)
                * sizeof(GpuGrouped32ControlWire)},
        {state.query_records_->buffer, 0,
            static_cast<VkDeviceSize>(state.query_record_count_)
                * sizeof(GpuGrouped32QueryRecordWire)},
        {state.query_minimizers_->buffer, 0,
            static_cast<VkDeviceSize>(state.query_minimizer_count_)
                * sizeof(GpuGrouped32QueryMinimizerWire)},
        {state.occurrences_.buffer, 0, state.occurrences_.bytes},
        {state.matches_.buffer, 0, state.matches_.bytes},
        {state.stream_state_.buffer, 0, state.stream_state_.bytes},
        {state.authorization_flags_.buffer, 0, state.authorization_flags_.bytes},
        {state.stream_state_.buffer, 0, state.stream_state_.bytes},
    }};
    const GpuGrouped32StreamPush push{
        .partition_count = inputs.descriptor_count,
        .control_count = inputs.payload_element_count,
        .position_count = 0u,
        .query_record_count = state.query_record_count_,
        .query_minimizer_count = state.query_minimizer_count_,
        .occurrence_count = state.occurrence_count_,
        .match_capacity = state.match_capacity_,
        .directory_entry_count = state.directory_entry_count_,
        .anchor_capacity = 0u,
        .max_occ = state.max_occ_,
        .kmer_size = state.kmer_size_,
        .group_ordinal = inputs.group_ordinal,
        .expected_control_groups = state.expected_control_groups_,
        .expected_position_groups = 0u,
        .query_identity_lo = static_cast<std::uint32_t>(state.query_identity_),
        .query_identity_hi = static_cast<std::uint32_t>(state.query_identity_ >> 32u),
        .control_state_identity_lo = static_cast<std::uint32_t>(
            state.control_state_identity_),
        .control_state_identity_hi = static_cast<std::uint32_t>(
            state.control_state_identity_ >> 32u),
    };
    const std::uint64_t pair_count =
        static_cast<std::uint64_t>(inputs.descriptor_count)
        * state.query_minimizer_count_;
    const std::uint64_t work_items = std::max<std::uint64_t>(
        inputs.descriptor_count, pair_count);
    const std::uint32_t groups = std::max<std::uint32_t>(1u,
        std::min<std::uint64_t>((work_items + 127u) / 128u,
            features_.max_workgroup_count_x));
    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0),
        "reset(grouped32_control)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
        "begin(grouped32_control)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, pipeline, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "end(grouped32_control)");
    submit_and_wait(device_, queue_, command_buffer_, fence_,
        "submit(grouped32_control)");
    const std::uint32_t delta = stream->matched_anchor_upper_bound - before_upper;
    return GpuGrouped32RuntimeControlReceipt{
        .device_uuid = features_.device_uuid,
        .receipt_identity = inputs.receipt_identity,
        .control_state_identity = state.control_state_identity_,
        .matched_anchor_upper_bound = delta,
        .status = stream->status,
        .accepted = stream->status == 0u,
    };
}

GpuGrouped32RuntimeAuthorizationReceipt
Runtime::run_grouped32_count_finalize_authorize(
    GpuGrouped32RuntimeStreamState& state,
    std::uint64_t receipt_identity)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available || receipt_identity == 0u
        || !std::all_of(state.control_groups_dispatched_.begin(),
            state.control_groups_dispatched_.end(),
            [](bool dispatched) noexcept { return dispatched; })
        || std::find(state.consumed_receipt_identities_.begin(),
                state.consumed_receipt_identities_.end(), receipt_identity)
            != state.consumed_receipt_identities_.end()) {
        throw VulkanRuntimeError("invalid grouped32 authorization receipt");
    }
    auto* stream = static_cast<GpuGrouped32StreamStateWire*>(
        state.stream_state_.mapped);
    if (stream->status != 0u || stream->completion != 0u
        || stream->control_groups_completed != state.expected_control_groups_) {
        throw VulkanRuntimeError("grouped32 controls are not complete");
    }
    state.consumed_receipt_identities_.push_back(receipt_identity);
    const PhaseBPipeline pipeline = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_grouped32_stream_authorize, 128u, 2u);
    const std::array<VkDescriptorBufferInfo, 9> infos{{
        {state.stream_state_.buffer, 0, state.stream_state_.bytes},
        {state.stream_state_.buffer, 0, state.stream_state_.bytes},
        {state.query_records_->buffer, 0,
            static_cast<VkDeviceSize>(state.query_record_count_)
                * sizeof(GpuGrouped32QueryRecordWire)},
        {state.query_minimizers_->buffer, 0,
            static_cast<VkDeviceSize>(state.query_minimizer_count_)
                * sizeof(GpuGrouped32QueryMinimizerWire)},
        {state.occurrences_.buffer, 0, state.occurrences_.bytes},
        {state.matches_.buffer, 0, state.matches_.bytes},
        {state.stream_state_.buffer, 0, state.stream_state_.bytes},
        {state.authorization_flags_.buffer, 0, state.authorization_flags_.bytes},
        {state.stream_state_.buffer, 0, state.stream_state_.bytes},
    }};
    const GpuGrouped32StreamPush push{
        .query_record_count = state.query_record_count_,
        .query_minimizer_count = state.query_minimizer_count_,
        .occurrence_count = state.occurrence_count_,
        .match_capacity = state.match_capacity_,
        .directory_entry_count = state.directory_entry_count_,
        .max_occ = state.max_occ_,
        .kmer_size = state.kmer_size_,
        .expected_control_groups = state.expected_control_groups_,
        .query_identity_lo = static_cast<std::uint32_t>(state.query_identity_),
        .query_identity_hi = static_cast<std::uint32_t>(state.query_identity_ >> 32u),
        .control_state_identity_lo = static_cast<std::uint32_t>(
            state.control_state_identity_),
        .control_state_identity_hi = static_cast<std::uint32_t>(
            state.control_state_identity_ >> 32u),
    };
    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0),
        "reset(grouped32_authorize)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
        "begin(grouped32_authorize)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, pipeline, infos, &push, sizeof(push), 1u);
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "end(grouped32_authorize)");
    submit_and_wait(device_, queue_, command_buffer_, fence_,
        "submit(grouped32_authorize)");
    GpuGrouped32RuntimeAuthorizationReceipt result{
        .device_uuid = features_.device_uuid,
        .receipt_identity = receipt_identity,
        .control_state_identity = state.control_state_identity_,
        .authorization_digest =
            static_cast<std::uint64_t>(stream->authorization_digest_lo)
            | (static_cast<std::uint64_t>(stream->authorization_digest_hi) << 32u),
        .authorized_anchor_count = stream->authorized_anchor_count,
        .status = stream->status,
        .accepted = stream->status == 0u && stream->completion == 1u,
    };
    const auto* flags = static_cast<const std::uint32_t*>(
        state.authorization_flags_.mapped);
    if (stream->authorized_partition_count > state.directory_entry_count_
        || result.authorized_anchor_count
            > stream->matched_anchor_upper_bound) {
        throw VulkanRuntimeError(
            "grouped32 authorization exceeded its matched upper bound");
    }
    result.authorized_directory_ordinals.reserve(
        stream->authorized_partition_count);
    for (std::uint32_t index = 0u;
         index < stream->authorized_partition_count; ++index) {
        result.authorized_directory_ordinals.push_back(flags[index]);
    }
    if (!result.accepted
        || result.authorization_digest == 0u
        || result.authorized_directory_ordinals.size()
            != stream->authorized_partition_count
        || !std::is_sorted(result.authorized_directory_ordinals.begin(),
            result.authorized_directory_ordinals.end())
        || std::adjacent_find(result.authorized_directory_ordinals.begin(),
            result.authorized_directory_ordinals.end())
            != result.authorized_directory_ordinals.end()
        || (!result.authorized_directory_ordinals.empty()
            && result.authorized_directory_ordinals.back()
                >= state.directory_entry_count_)) {
        throw VulkanRuntimeError("grouped32 authorization device proof rejected");
    }
    const std::uint64_t anchor_bytes =
        result.authorized_anchor_count
        * sizeof(GpuGrouped32GlobalAnchorWire);
    if (result.authorized_anchor_count
            > (std::numeric_limits<std::uint32_t>::max)()
        || (result.authorized_anchor_count != 0u
            && anchor_bytes / sizeof(GpuGrouped32GlobalAnchorWire)
                != result.authorized_anchor_count)
        || anchor_bytes > features_.max_storage_buffer_range) {
        throw VulkanRuntimeError("grouped32 authorized anchor capacity overflow");
    }
    const std::uint64_t requested_anchor_bytes = std::max<std::uint64_t>(
        anchor_bytes, sizeof(GpuGrouped32GlobalAnchorWire));
    const std::uint64_t new_allocation = buffer_memory_requirement_locked(
        requested_anchor_bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const std::uint64_t prior_admitted_bytes = state.permit_.bytes();
    const std::uint64_t peak = sat_add(prior_admitted_bytes, new_allocation);
    if (peak == (std::numeric_limits<std::uint64_t>::max)()
        || !state.permit_.grow_to(peak)) {
        throw VulkanRuntimeError(
            "grouped32 exact anchor capacity memory admission failed");
    }
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Buffer exact_anchors = create_device_buffer(requested_anchor_bytes, usage);
    if (exact_anchors.allocation_bytes > new_allocation
        || exact_anchors.mapped == nullptr
        || !exact_anchors.host_visible_coherent()) {
        throw VulkanRuntimeError(
            "grouped32 exact anchors require coherent mapped device memory");
    }
    std::memset(exact_anchors.mapped, 0,
        static_cast<std::size_t>(exact_anchors.bytes));
    state.anchors_ = std::move(exact_anchors);
    state.permit_.shrink_to(
        prior_admitted_bytes + state.anchors_.allocation_bytes);
    state.authorization_digest_ = result.authorization_digest;
    state.authorized_anchor_count_ = result.authorized_anchor_count;
    state.target_buffer_identity_ =
        grouped32_stream_identity_.fetch_add(1u, std::memory_order_relaxed);
    if (state.target_buffer_identity_ == 0u) {
        state.target_buffer_identity_ =
            grouped32_stream_identity_.fetch_add(1u, std::memory_order_relaxed);
    }
    result.target_buffer_identity = state.target_buffer_identity_;
    result.anchor_bytes = anchor_bytes;
    return result;
}

void Runtime::bind_grouped32_expected_position_groups(
    GpuGrouped32RuntimeStreamState& state,
    std::uint32_t expected_position_groups)
{
    std::lock_guard<std::mutex> guard(mu_);
    const auto* stream = static_cast<const GpuGrouped32StreamStateWire*>(
        state.stream_state_.mapped);
    if (stream->status != 0u || stream->completion != 1u
        || state.target_buffer_identity_ == 0u
        || state.position_groups_bound_) {
        throw VulkanRuntimeError(
            "grouped32 position-group binding is not admissible");
    }
    state.expected_position_groups_ = expected_position_groups;
    state.position_groups_dispatched_.assign(
        expected_position_groups, false);
    state.position_groups_bound_ = true;
}

GpuGrouped32RuntimePositionReceipt
Runtime::run_grouped32_position_validate_emit(
    GpuGrouped32RuntimeStreamState& state,
    const GpuGrouped32RawGroupInputs& inputs)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available || inputs.receipt_identity == 0u
        || inputs.descriptors == nullptr || !inputs.descriptors->valid()
        || inputs.payload == nullptr || !inputs.payload->valid()
        || inputs.descriptor_count == 0u
        || inputs.payload_element_count == 0u
        || inputs.group_ordinal >= state.expected_position_groups_
        || !state.position_groups_bound_
        || state.position_groups_dispatched_[inputs.group_ordinal]
        || std::find(state.consumed_receipt_identities_.begin(),
                state.consumed_receipt_identities_.end(),
                inputs.receipt_identity)
            != state.consumed_receipt_identities_.end()
        || static_cast<std::uint64_t>(inputs.descriptor_count)
                * sizeof(GpuGrouped32PartitionWire)
            > inputs.descriptors->bytes
        || static_cast<std::uint64_t>(inputs.payload_element_count)
                * sizeof(GpuGrouped32PositionWire)
            > inputs.payload->bytes) {
        throw VulkanRuntimeError("invalid grouped32 raw position group");
    }
    auto* stream = static_cast<GpuGrouped32StreamStateWire*>(
        state.stream_state_.mapped);
    if (stream->status != 0u || stream->completion != 1u
        || stream->position_groups_completed >= state.expected_position_groups_) {
        throw VulkanRuntimeError("grouped32 position state is not dispatchable");
    }
    state.position_groups_dispatched_[inputs.group_ordinal] = true;
    state.consumed_receipt_identities_.push_back(inputs.receipt_identity);
    const std::uint32_t before = stream->emitted_anchor_count;
    const PhaseBPipeline pipeline = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_grouped32_stream_position, 128u, 2u);
    const std::array<VkDescriptorBufferInfo, 9> infos{{
        {inputs.descriptors->buffer, 0,
            static_cast<VkDeviceSize>(inputs.descriptor_count)
                * sizeof(GpuGrouped32PartitionWire)},
        {inputs.payload->buffer, 0,
            static_cast<VkDeviceSize>(inputs.payload_element_count)
                * sizeof(GpuGrouped32PositionWire)},
        {state.query_records_->buffer, 0,
            static_cast<VkDeviceSize>(state.query_record_count_)
                * sizeof(GpuGrouped32QueryRecordWire)},
        {state.query_minimizers_->buffer, 0,
            static_cast<VkDeviceSize>(state.query_minimizer_count_)
                * sizeof(GpuGrouped32QueryMinimizerWire)},
        {state.occurrences_.buffer, 0, state.occurrences_.bytes},
        {state.matches_.buffer, 0, state.matches_.bytes},
        {state.stream_state_.buffer, 0, state.stream_state_.bytes},
        {state.authorization_flags_.buffer, 0, state.authorization_flags_.bytes},
        {state.anchors_.buffer, 0, state.anchors_.bytes},
    }};
    const GpuGrouped32StreamPush push{
        .partition_count = inputs.descriptor_count,
        .position_count = inputs.payload_element_count,
        .query_record_count = state.query_record_count_,
        .query_minimizer_count = state.query_minimizer_count_,
        .occurrence_count = state.occurrence_count_,
        .match_capacity = state.match_capacity_,
        .directory_entry_count = state.directory_entry_count_,
        .anchor_capacity = static_cast<std::uint32_t>(
            state.authorized_anchor_count_),
        .max_occ = state.max_occ_,
        .kmer_size = state.kmer_size_,
        .group_ordinal = inputs.group_ordinal,
        .expected_control_groups = state.expected_control_groups_,
        .expected_position_groups = state.expected_position_groups_,
        .query_identity_lo = static_cast<std::uint32_t>(state.query_identity_),
        .query_identity_hi = static_cast<std::uint32_t>(state.query_identity_ >> 32u),
        .control_state_identity_lo = static_cast<std::uint32_t>(
            state.control_state_identity_),
        .control_state_identity_hi = static_cast<std::uint32_t>(
            state.control_state_identity_ >> 32u),
        .target_buffer_identity_lo = static_cast<std::uint32_t>(
            state.target_buffer_identity_),
        .target_buffer_identity_hi = static_cast<std::uint32_t>(
            state.target_buffer_identity_ >> 32u),
    };
    const std::uint64_t work_items = std::max<std::uint64_t>(
        inputs.descriptor_count, stream->match_count);
    const std::uint32_t groups = std::max<std::uint32_t>(1u,
        std::min<std::uint64_t>((work_items + 127u) / 128u,
            features_.max_workgroup_count_x));
    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0),
        "reset(grouped32_position)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
        "begin(grouped32_position)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, pipeline, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "end(grouped32_position)");
    submit_and_wait(device_, queue_, command_buffer_, fence_,
        "submit(grouped32_position)");
    return GpuGrouped32RuntimePositionReceipt{
        .device_uuid = features_.device_uuid,
        .receipt_identity = inputs.receipt_identity,
        .target_buffer_identity = state.target_buffer_identity_,
        .emitted_anchor_count =
            stream->emitted_anchor_count - before,
        .status = stream->status,
        .accepted = stream->status == 0u,
    };
}

GpuGrouped32RuntimeSealReceipt Runtime::run_grouped32_target_seal(
    GpuGrouped32RuntimeStreamState& state,
    std::uint64_t receipt_identity)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available || receipt_identity == 0u
        || !state.position_groups_bound_
        || !std::all_of(state.position_groups_dispatched_.begin(),
            state.position_groups_dispatched_.end(),
            [](bool dispatched) noexcept { return dispatched; })
        || std::find(state.consumed_receipt_identities_.begin(),
                state.consumed_receipt_identities_.end(), receipt_identity)
            != state.consumed_receipt_identities_.end()) {
        throw VulkanRuntimeError("invalid grouped32 target-seal receipt");
    }
    auto* stream = static_cast<GpuGrouped32StreamStateWire*>(
        state.stream_state_.mapped);
    if (stream->status != 0u || stream->completion != 1u
        || stream->position_groups_completed != state.expected_position_groups_) {
        throw VulkanRuntimeError("grouped32 positions are not complete");
    }
    state.consumed_receipt_identities_.push_back(receipt_identity);
    const PhaseBPipeline pipeline = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_grouped32_stream_seal, 128u, 2u);
    const std::array<VkDescriptorBufferInfo, 9> infos{{
        {state.stream_state_.buffer, 0, state.stream_state_.bytes},
        {state.stream_state_.buffer, 0, state.stream_state_.bytes},
        {state.query_records_->buffer, 0,
            static_cast<VkDeviceSize>(state.query_record_count_)
                * sizeof(GpuGrouped32QueryRecordWire)},
        {state.query_minimizers_->buffer, 0,
            static_cast<VkDeviceSize>(state.query_minimizer_count_)
                * sizeof(GpuGrouped32QueryMinimizerWire)},
        {state.occurrences_.buffer, 0, state.occurrences_.bytes},
        {state.matches_.buffer, 0, state.matches_.bytes},
        {state.stream_state_.buffer, 0, state.stream_state_.bytes},
        {state.authorization_flags_.buffer, 0, state.authorization_flags_.bytes},
        {state.anchors_.buffer, 0, state.anchors_.bytes},
    }};
    const GpuGrouped32StreamPush push{
        .query_record_count = state.query_record_count_,
        .query_minimizer_count = state.query_minimizer_count_,
        .occurrence_count = state.occurrence_count_,
        .match_capacity = state.match_capacity_,
        .directory_entry_count = state.directory_entry_count_,
        .anchor_capacity = static_cast<std::uint32_t>(
            state.authorized_anchor_count_),
        .max_occ = state.max_occ_,
        .kmer_size = state.kmer_size_,
        .expected_control_groups = state.expected_control_groups_,
        .expected_position_groups = state.expected_position_groups_,
        .query_identity_lo = static_cast<std::uint32_t>(state.query_identity_),
        .query_identity_hi = static_cast<std::uint32_t>(state.query_identity_ >> 32u),
        .control_state_identity_lo = static_cast<std::uint32_t>(
            state.control_state_identity_),
        .control_state_identity_hi = static_cast<std::uint32_t>(
            state.control_state_identity_ >> 32u),
        .target_buffer_identity_lo = static_cast<std::uint32_t>(
            state.target_buffer_identity_),
        .target_buffer_identity_hi = static_cast<std::uint32_t>(
            state.target_buffer_identity_ >> 32u),
    };
    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0),
        "reset(grouped32_seal)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
        "begin(grouped32_seal)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, pipeline, infos, &push, sizeof(push), 1u);
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "end(grouped32_seal)");
    submit_and_wait(device_, queue_, command_buffer_, fence_,
        "submit(grouped32_seal)");
    const std::uint64_t digest =
        static_cast<std::uint64_t>(stream->anchor_digest_lo)
        | (static_cast<std::uint64_t>(stream->anchor_digest_hi) << 32u);
    return GpuGrouped32RuntimeSealReceipt{
        .device_uuid = features_.device_uuid,
        .receipt_identity = receipt_identity,
        .authorization_digest = state.authorization_digest_,
        .target_buffer_identity = state.target_buffer_identity_,
        .emitted_anchor_count = stream->emitted_anchor_count,
        .anchor_digest = digest,
        .status = stream->status,
        .accepted = stream->status == 0u && stream->completion == 2u
            && (state.authorized_anchor_count_ == 0u || digest != 0u),
    };
}

SeedLookupResult Runtime::run_phase_c_seed_roundtrip(
    const PhaseCSeedInputs& inputs,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::int32_t max_occ)
{
    std::lock_guard<std::mutex> guard(mu_);
    SeedLookupResult result;
    if (!features_.available) throw VulkanRuntimeError("run_phase_c_seed_roundtrip on unavailable device");
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw VulkanRuntimeError("phase_c seed missing resident index buffers");
    }
    const std::uint32_t mc = inputs.minimizer_count;
    GpuArena& arena = phase_c_arena();
    if (mc == 0u || max_occ < 0) {
        if (!arena.release_idle_capacity()) {
            throw VulkanRuntimeError(
                "phase_c seed idle arena release found live slices");
        }
        return result;
    }
    const std::uint32_t num_blocks = ceil_div_u32(mc, kPhaseCScanBlock);
    const std::uint64_t min_words_bytes = inputs.device_minimizer_words == nullptr
        ? std::max<std::uint64_t>(minimizer_device_bytes(inputs.minimizer_word_kind, inputs.minimizer_words32_per_record, mc), 4u)
        : 0u;
    // Each allocate_device call rounds the offset up to kPhaseCSlotOffsetAlign
    // (256), so the five sub-allocations below can need up to 5*256 bytes of
    // alignment padding on top of their byte footprint. Reserve enough slack for
    // that — a flat +256 (the old value) overflowed the arena on a cold call
    // (the last allocation, minw_s, came back invalid and minw_s.buffer->buffer
    // null-dereferenced), masked only because a warmer arena had grown larger.
    std::uint64_t dt = std::uint64_t{mc} * 4u * 2u + std::uint64_t{num_blocks} * 4u + 16u
        + min_words_bytes + 6u * std::uint64_t{kPhaseCSlotOffsetAlign};
    if (!arena.ensure_capacity(dt, 0, 0)) {
        // ensure_capacity never advances a cursor.  Drop any partial/high-water
        // backing before reporting the failure so a retry starts cold and does
        // not inherit memory from a failed request.
        if (!arena.release_idle_capacity()) {
            throw VulkanRuntimeError(
                "phase_c seed failed grow left live arena slices");
        }
        throw VulkanRuntimeError("phase_c seed arena grow failed");
    }
    const GpuArenaMark mark = arena.mark();
    struct SeedArenaLifetime {
        GpuArena& arena;
        GpuArenaMark mark;
        bool released = false;

        ~SeedArenaLifetime() noexcept {
            if (!released) {
                // Exception cleanup must leave a retryable arena.  Destructors
                // cannot report the invariant failure, but every explicit
                // return below uses release_or_throw() and does report it.
                arena.rewind(mark);
                (void)arena.release_idle_capacity();
            }
        }

        void release_or_throw() {
            arena.rewind(mark);
            if (!arena.release_idle_capacity()) {
                throw VulkanRuntimeError(
                    "phase_c seed idle arena release found live slices");
            }
            released = true;
        }
    } seed_arena_lifetime{arena, mark};

    const GpuArenaSlice counts_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const GpuArenaSlice prefix_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const GpuArenaSlice blocks_s = arena.allocate_device(std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const GpuArenaSlice qrec_s = arena.allocate_device(16u, 16u);
    GpuArenaSlice minw_s{};
    if (inputs.device_minimizer_words == nullptr) minw_s = arena.allocate_device(min_words_bytes, 16u);
    if (!counts_s.valid() || !prefix_s.valid() || !blocks_s.valid() || !qrec_s.valid()
        || (inputs.device_minimizer_words == nullptr && !minw_s.valid())) {
        seed_arena_lifetime.release_or_throw();
        throw VulkanRuntimeError("phase_c seed arena oversubscribed");
    }
    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    const VkBuffer min_words_buffer = inputs.device_minimizer_words != nullptr
        ? inputs.device_minimizer_words->buffer
        : minw_s.buffer->buffer;
    const std::uint64_t min_words_offset = inputs.device_minimizer_words != nullptr ? 0u : minw_s.offset;
    const std::uint64_t min_words_range = inputs.device_minimizer_words != nullptr ? VK_WHOLE_SIZE : minw_s.size;
    if (inputs.device_minimizer_words == nullptr) {
        std::memset(base + minw_s.offset, 0, static_cast<std::size_t>(minw_s.size));
        if (!inputs.host_minimizer_bytes.empty()) {
            std::memcpy(base + minw_s.offset, inputs.host_minimizer_bytes.data(), inputs.host_minimizer_bytes.size());
        }
    }
    const HostQRec qrec{0u, mc, static_cast<std::uint32_t>(query_len), 0u};
    std::memcpy(base + qrec_s.offset, &qrec, sizeof(qrec));

    const PhaseBPipeline count_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_count, 128u, 1u);
    const PhaseBPipeline emit_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_emit, 128u, 1u);
    const PhaseBPipeline scanb = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scana = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);

    const std::uint32_t target_offset_lo =
        static_cast<std::uint32_t>(inputs.target_position_offset & 0xffffffffull);
    const std::uint32_t target_offset_hi =
        static_cast<std::uint32_t>(inputs.target_position_offset >> 32);
    const PhaseCAnchorSortKeyPlan anchor_sort_plan =
        phase_c_anchor_sort_key_plan(inputs.target_len, inputs.target_position_offset);
    SeedPushC cpush{inputs.minimizer_words32_per_record, inputs.minimizer_word_kind,
        inputs.hash_shift, inputs.pos_bits, kmer_size, inputs.slot_mask,
        static_cast<std::uint32_t>(max_occ), 0u, 1u, inputs.bucket_count,
        target_offset_lo, target_offset_hi, anchor_sort_plan.target_ref_bits};
    const std::uint32_t seed_groups = std::max<std::uint32_t>(
        1u,
        std::min(ceil_div_u32(mc, 128u), features_.max_workgroup_count_x));

    // --- Submit 1: seed count + prefix scan ---
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_seed_count");
        auto _rs = bioseq::instr::region(_rd);
        reset_phase_c_pool_locked();
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},  // anchors (unused in count)
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},  // anchor_keys (unused in count)
        }};
        const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {blocks_s.buffer->buffer, blocks_s.offset, blocks_s.size},
        }};
        ScanPushC s0{mc, kPhaseCScanBlock, num_blocks, 0u};
        ScanPushC s1{mc, kPhaseCScanBlock, num_blocks, 1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed1)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed1)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, count_pipe, seed_infos, &cpush, sizeof(cpush), seed_groups);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, scanb, scan_infos, &s0, sizeof(s0), num_blocks);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, scana, scan_infos, &s0, sizeof(s0), 1u);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, scana, scan_infos, &s1, sizeof(s1), ceil_div_u32(mc, 128u));
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed1)");
    }
    result.seed_lookup_dispatches = 1u;
    result.open_addressed_lookup_dispatches =
        inputs.bucket_count == 0u ? 1u : 0u;
    result.sorted_dense_lookup_dispatches =
        inputs.bucket_count != 0u ? 1u : 0u;
    std::uint32_t plast = 0, clast = 0;
    std::memcpy(&plast, base + prefix_s.offset + (mc - 1u) * 4u, 4);
    std::memcpy(&clast, base + counts_s.offset + (mc - 1u) * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(plast) + clast;
    if (total == 0u) {
        seed_arena_lifetime.release_or_throw();
        return result;
    }
    if (total > std::numeric_limits<std::uint32_t>::max()) {
        seed_arena_lifetime.release_or_throw();
        throw VulkanRuntimeError("phase_c seed anchor count overflow");
    }

    // --- Submit 2: seed emit + device anchor sort by ref_bits ---
    // The exact anchor count is only known after the count submit, so the anchor
    // and sort-scratch buffers are sized exactly here as standalone device
    // buffers (the count/prefix arena slices from submit 1 must stay live, so the
    // arena is never grown mid-op). They are wrapped as offset-0 GpuArenaSlices
    // for the shared dispatch/sort recorders.
    const std::uint64_t anchor_u64 = total * 3u;          // 24-byte anchors as 3 u64
    const WgPlan awg = plan_workgroups(total, features_.max_workgroup_count_x);
    const std::uint64_t exact_reserve =
        phase_c_seed_exact_reserved_bytes(total, awg.num_workgroups);
    auto exact_permit =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(exact_reserve);
    if (!exact_permit) {
        auto& admission =
            bioseq::parallelism::MemoryAdmissionController::instance();
        seed_arena_lifetime.release_or_throw();
        throw VulkanRuntimeError(fmt::format(
            "phase_c seed exact anchor memory admission failed for {} bytes "
            "(anchors={} workgroups={} reserved={} allocatable={})",
            exact_reserve,
            total,
            awg.num_workgroups,
            admission.reserved_bytes(),
            admission.allocatable_after_reservations()));
    }
    Buffer anchors_buf = create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
    Buffer akeys_buf = create_device_buffer(total * 8u, kPhaseBBufferUsage);
    Buffer anchors_scr_buf = create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
    Buffer akeys_scr_buf = create_device_buffer(total * 8u, kPhaseBBufferUsage);
    Buffer ahist_buf = create_device_buffer(std::uint64_t{awg.num_workgroups} * 256u * 4u, kPhaseBBufferUsage);
    if (anchors_buf.mapped == nullptr) {
        seed_arena_lifetime.release_or_throw();
        throw VulkanRuntimeError("phase_c seed non-unified memory");
    }
    const GpuArenaSlice anchors_s{&anchors_buf, 0, anchors_buf.bytes};
    const GpuArenaSlice akeys_s{&akeys_buf, 0, akeys_buf.bytes};
    const GpuArenaSlice anchors_scr_s{&anchors_scr_buf, 0, anchors_scr_buf.bytes};
    const GpuArenaSlice akeys_scr_s{&akeys_scr_buf, 0, akeys_scr_buf.bytes};
    const GpuArenaSlice ahist_s{&ahist_buf, 0, ahist_buf.bytes};

    bool sorted_in_scratch = false;
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_seed_emit");
        auto _rs = bioseq::instr::region(_rd);
        reset_phase_c_pool_locked();
        SeedPushC epush = cpush; epush.emit_mode = 1u;
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {anchors_s.buffer->buffer, anchors_s.offset, anchors_s.size},
            {akeys_s.buffer->buffer, akeys_s.offset, akeys_s.size},
        }};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed2)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed2)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, emit_pipe, seed_infos, &epush, sizeof(epush), seed_groups);
        record_compute_barrier(command_buffer_);
        GpuSortPlan sort_plan{total, 8u, 24u, 8u, anchor_sort_plan.pass_count};
        record_radix_sort_locked(command_buffer_, akeys_s, anchors_s, akeys_scr_s, anchors_scr_s, ahist_s,
                                 sort_plan, awg.num_workgroups, awg.elems_per_wg, 3u, sorted_in_scratch);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed2)");
    }
    const GpuArenaSlice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
    result.count = total;
    result.host_bytes.resize(static_cast<std::size_t>(total * 24u));
    std::memcpy(result.host_bytes.data(),
                static_cast<const std::byte*>(sorted_anchors.buffer->mapped) + sorted_anchors.offset,
                static_cast<std::size_t>(total * 24u));
    result.sort_proof.radix_sort_operations = 1u;
    result.sort_proof.radix_anchor_count = total;
    result.sort_proof.radix_passes = anchor_sort_plan.pass_count;
    result.sort_proof.radix_histogram_dispatches =
        anchor_sort_plan.pass_count;
    result.sort_proof.radix_scan_dispatches = anchor_sort_plan.pass_count;
    result.sort_proof.radix_scatter_dispatches =
        anchor_sort_plan.pass_count;

    seed_arena_lifetime.release_or_throw();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

}  // namespace bioseq::gpu::vulkan

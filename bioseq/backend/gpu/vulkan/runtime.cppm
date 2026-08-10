/**
 * @file runtime.cppm
 * @brief Vulkan/MoltenVK runtime for the Vulkan chaining tiled-DP chaining kernel.
 *
 * Owns the Vulkan instance / physical device / logical device / compute queue,
 * the fixed chaining descriptor-set + pipeline layout, a per-tile-size compute
 * pipeline cache, and one reusable command buffer + fence. Exposes exactly one
 * hot entry point — `run_chaining_tiled_dp_roundtrip` — which records every
 * H2D copy, the dispatch, and every D2H copy under a single queue lock.
 *
 * Device selection, feature probing, and all per-run failures are runtime-side
 * decisions: init failures set `features_.available == false` with an
 * initialization error, and per-run failures throw `vulkan_runtime_error`. Vulkan chaining
 * targets unified memory (Apple Silicon / MoltenVK): device-local buffers come
 * back host-visible+coherent, so the round-trip writes/reads them directly
 * without a staging bounce.
 */
module;
#include <libassert/assert.hpp>

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
export import bioseq.gpu.vulkan.abi;
export import bioseq.prefilter.types;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.instrumentation;

export namespace bioseq::gpu::vulkan {

/// @brief Process-wide owner of the Vulkan device, queues, pipelines, and buffers.
class runtime;

#ifdef BIOSEQ_TEST_INDEXES
/** @brief Test-build-only latch around the real GPU frontend upload/submit/fence path.
 * Normal builds do not contain this API or its branches. */
enum class vulkan_submission_test_fault : std::uint8_t {
  none,
  cancellation_after_submit,
  upload_failure_after_submit,
};

/// @brief Observable state of the test-only submission fault seam.
struct vulkan_submission_test_snapshot {
  vulkan_submission_test_fault fault{vulkan_submission_test_fault::none};
  std::uint64_t upload_latches = 0u;
  std::uint64_t submissions = 0u;
  std::uint64_t fence_drains = 0u;
  std::uint64_t delivered_faults = 0u;
  bool waiting_after_submit = false;
  bool waiting_after_drain = false;
  bool cancellation_requested = false;
  bool upload_failure_latched = false;
};

/// @brief Arms one test-only fault for the next Vulkan submission.
void arm_vulkan_submission_test_seam(vulkan_submission_test_fault fault) noexcept;
/// @brief Releases a test submission waiting at its fence seam.
void release_vulkan_submission_test_fence() noexcept;
/// @brief Releases delivery of a fault latched by the test seam.
void release_vulkan_submission_test_delivery() noexcept;
/// @brief Restores the test submission seam to its idle state.
void reset_vulkan_submission_test_seam() noexcept;
/// @brief Captures a consistent snapshot of the test submission seam.
/// @return The computed result.
[[nodiscard]] vulkan_submission_test_snapshot
capture_vulkan_submission_test_snapshot() noexcept;
#endif

/// @brief Host-owned results from one batched alignment and CIGAR dispatch.
struct alignment_cigar_batch_result {
  std::vector<gpu_alignment_cigar_result_wire> results{};
  std::vector<std::uint32_t> packed_cigar{};
};

/// @brief Capabilities probed once at init. `available == false` means there is no
/// usable Vulkan device and selected GPU algorithms must fail.
struct vulkan_feature_set {
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

inline constexpr std::size_t k_rank_prefilter_replay_route_count = 7u;

/// @brief Resolve the fixed recorded-command slot for one rank-prefilter shader and
/// push mode. Keeping this mapping pure makes the seven non-aliasing replay
/// identities independently testable without a Vulkan device.
/// @param kind Rank-prefilter shader identity.
/// @param push_mode Shader push mode encoded in the recorded command.
/// @return The matching replay slot, or no value for an unsupported route.
[[nodiscard]] constexpr std::optional<std::size_t>
rank_prefilter_replay_index(
    shader_kind kind,
    std::uint32_t push_mode) noexcept
{
    if (kind == shader_kind::sneaky_snake_rank_batch) {
        if (push_mode == 1u) return 0u;
        if (push_mode == 8u) return 1u;
        return std::nullopt;
    }
    if (push_mode != 0u) return std::nullopt;
    switch (kind) {
    case shader_kind::gatekeeper_rank_batch:
        return 2u;
    case shader_kind::gatekeeper_rank_batch_l32_e2:
        return 3u;
    case shader_kind::gatekeeper_rank_batch_l32_e4:
        return 4u;
    case shader_kind::gatekeeper_rank_batch_l64_e2:
        return 5u;
    case shader_kind::gatekeeper_rank_batch_l64_e4:
        return 6u;
    default:
        return std::nullopt;
    }
}

/// @brief Shader identity and replay slot selected for one GateKeeper batch.
struct gatekeeper_rank_batch_route {
    shader_kind kind = shader_kind::gatekeeper_rank_batch;
    std::size_t replay_index = 2u;

    constexpr bool operator==(const gatekeeper_rank_batch_route&) const noexcept =
        default;
};

/// @brief Select a specialized GateKeeper shader only for a homogeneous, word-aligned
/// production batch. Every other shape deliberately retains the generic key.
/// @param jobs Borrowed prefilter jobs whose geometry selects the route.
/// @return The specialized or generic GateKeeper route.
[[nodiscard]] inline gatekeeper_rank_batch_route
resolve_gatekeeper_rank_batch_route(
    std::span<const bioseq::prefilter::prefilter_job> jobs) noexcept
{
    if (jobs.empty()) return {};

    const std::uint32_t length = jobs.front().query_length;
    const std::uint32_t max_edits = jobs.front().max_edits;
    bool homogeneous_aligned_production =
        jobs.front().target_length == length
        && (length == 32u || length == 64u)
        && (max_edits == 2u || max_edits == 4u);
    for (const auto& job : jobs) {
        homogeneous_aligned_production =
            homogeneous_aligned_production
            && job.query_length == length
            && job.target_length == length
            && job.max_edits == max_edits
            && (job.query_offset & 3u) == 0u
            && (job.target_offset & 3u) == 0u;
    }
    if (!homogeneous_aligned_production) return {};

    if (length == 32u && max_edits == 2u) {
        return {
            .kind = shader_kind::gatekeeper_rank_batch_l32_e2,
            .replay_index = 3u,
        };
    }
    if (length == 32u) {
        return {
            .kind = shader_kind::gatekeeper_rank_batch_l32_e4,
            .replay_index = 4u,
        };
    }
    if (max_edits == 2u) {
        return {
            .kind = shader_kind::gatekeeper_rank_batch_l64_e2,
            .replay_index = 5u,
        };
    }
    return {
        .kind = shader_kind::gatekeeper_rank_batch_l64_e4,
        .replay_index = 6u,
    };
}

/// @brief Canonical allocation-free Vulkan driver identity used by SQL/report receipt.
/// Numeric fields avoid unstable marketing strings and preserve the exact
/// pipeline-cache compatibility UUID byte order reported by Vulkan.
/// @param features Immutable Vulkan device feature snapshot.
/// @return A fixed-width canonical driver identity string.
[[nodiscard]] inline std::array<char, 128>
canonical_vulkan_driver_identity(const vulkan_feature_set& features) noexcept {
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

/// @brief RAII owner for one (VkBuffer, VkDeviceMemory) pair. Move-only; destruction
/// routes through `runtime::destroy_buffer` (the single cleanup path).
struct buffer {
    buffer() = default;
    buffer(runtime* owner, VkBuffer buffer_handle, VkDeviceMemory memory,
           std::uint64_t bytes, std::uint64_t allocation_bytes,
           VkMemoryPropertyFlags memory_properties,
           bool process_footprint_reflected,
           void* mapped) noexcept
        : owner(owner), buffer_handle(buffer_handle), memory(memory), bytes(bytes),
          allocation_bytes(allocation_bytes),
          memory_properties(memory_properties),
          process_footprint_reflected(process_footprint_reflected),
          mapped(mapped) {}
    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;
    buffer(buffer&& other) noexcept { move_from(other); }
    buffer& operator=(buffer&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }
    ~buffer() { reset(); }

    void reset() noexcept;
    [[nodiscard]] bool is_valid() const noexcept {
        return buffer_handle != VK_NULL_HANDLE;
    }
    [[nodiscard]] bool is_host_visible_coherent() const noexcept {
        constexpr VkMemoryPropertyFlags required =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        return (memory_properties & required) == required;
    }

    runtime* owner = nullptr;
    VkBuffer buffer_handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint64_t bytes = 0;
    std::uint64_t allocation_bytes = 0;
    VkMemoryPropertyFlags memory_properties = 0;
    bool process_footprint_reflected = false;
    void* mapped = nullptr;

private:
    void move_from(buffer& other) noexcept {
        owner = other.owner;
        buffer_handle = other.buffer_handle;
        memory = other.memory;
        bytes = other.bytes;
        allocation_bytes = other.allocation_bytes;
        memory_properties = other.memory_properties;
        process_footprint_reflected = other.process_footprint_reflected;
        mapped = other.mapped;
        other.owner = nullptr;
        other.buffer_handle = VK_NULL_HANDLE;
        other.memory = VK_NULL_HANDLE;
        other.bytes = 0;
        other.allocation_bytes = 0;
        other.memory_properties = 0;
        other.process_footprint_reflected = false;
        other.mapped = nullptr;
    }
};

/// @brief Converts a Vulkan dispatchable or non-dispatchable handle to stable bits.
/// @param handle Vulkan handle to encode without dereferencing it.
/// @return Stable unsigned handle bits.
template <class p_handle_type>
[[nodiscard]] inline std::uint64_t vulkan_handle_identity(
    const p_handle_type handle) noexcept {
    static_assert(sizeof(p_handle_type) <= sizeof(std::uint64_t));
    std::uint64_t identity = 0u;
    std::memcpy(&identity, &handle, sizeof(handle));
    return identity;
}

/// @brief Immutable snapshot of one runtime-owned allocation.  The object address is
/// insufficient because buffer is move-assignable; operation receipt therefore
/// binds its owner, Vulkan allocation handles, and physical/logical sizes too.
struct runtime_buffer_identity {
    const buffer* object = nullptr;
    const runtime* owner = nullptr;
    std::uint64_t buffer_handle = 0u;
    std::uint64_t memory_handle = 0u;
    std::uint64_t bytes = 0u;
    std::uint64_t allocation_bytes = 0u;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return object != nullptr && owner != nullptr
            && buffer_handle != 0u && memory_handle != 0u
            && bytes != 0u && allocation_bytes >= bytes;
    }
    constexpr bool operator==(const runtime_buffer_identity&) const noexcept =
        default;
};

/// @brief Captures the allocation identity required by operation receipts.
/// @param allocation Runtime-owned allocation to capture.
/// @return The allocation identity, or an empty identity for null input.
[[nodiscard]] inline runtime_buffer_identity make_runtime_buffer_identity(
    const buffer* allocation) noexcept {
    if (allocation == nullptr) return {};
    return runtime_buffer_identity{
        .object = allocation,
        .owner = allocation->owner,
        .buffer_handle = vulkan_handle_identity(allocation->buffer_handle),
        .memory_handle = vulkan_handle_identity(allocation->memory),
        .bytes = allocation->bytes,
        .allocation_bytes = allocation->allocation_bytes,
    };
}

/// @brief One independent hard chaining range = one dispatched workgroup.
struct gpu_chain_range {
    std::uint32_t begin = 0;
    std::uint32_t length = 0;
};

/// @brief Thrown on any per-run Vulkan failure once the device is usable. Selected
/// Vulkan algorithms must propagate this rather than silently using CPU work.
class vulkan_runtime_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Optional supervisor-owned provider for precompiled MoltenVK metallibs.
/// When installed, runtime pipeline creation asks this hook for a shader module
/// instead of invoking glslang/spirv-cross on the backend worker thread.
struct runtime_shader_provider {
    using prepare_function = std::expected<void, std::string> (*)();
    using load_function = std::expected<shader_module_load, std::string> (*)(
        VkDevice device, const shader_variant_key& key, PFN_vkCreateShaderModule create_shader_module);
    prepare_function prepare_runtime = nullptr;
    load_function load_module = nullptr;
};

/// @brief Returns the process-owned optional shader-provider slot.
/// @return Mutable reference to the process-owned provider pointer.
runtime_shader_provider*& runtime_shader_provider_slot() noexcept {
    static runtime_shader_provider* provider = nullptr;
    return provider;
}

/// @brief Installs the supervisor-provided shader module source.
/// @param provider Optional process-owned provider; null removes the provider.
void install_runtime_shader_provider(runtime_shader_provider* provider) noexcept {
    runtime_shader_provider_slot() = provider;
}

/// @brief Validates the canonical lowercase hexadecimal runtime-cache digest.
/// @param digest Candidate cache digest.
/// @return True only for a nonzero 16-character lowercase hexadecimal digest.
[[nodiscard]] inline bool is_runtime_cache_digest_valid(
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

/// @brief One exact Metal binary archive inside the process-owned sealed seed image.
/// The ordinal binds runtime staging to retained bytes rather than a cache
/// pathname that could be replaced after supervisor validation.
struct runtime_metal_archive_spec {
    std::uint32_t artifact_ordinal =
        (std::numeric_limits<std::uint32_t>::max)();
    std::string artifact_filename;
    std::uint64_t size = 0u;
    std::array<std::uint8_t, 32> sha256{};
    std::string negative_error_domain;
    std::int64_t negative_error_code = 0;

    [[nodiscard]] bool is_valid() const noexcept {
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
    [[nodiscard]] bool operator==(const runtime_metal_archive_spec&) const noexcept = default;
};

/// @brief Immutable input installed by the PostgreSQL cache owner before runtime is
/// constructed.  It intentionally contains no directory: an unlisted archive
/// can never become eligible merely by appearing beside a listed one.
struct runtime_metal_archive_configuration {
    std::shared_ptr<const sealed_shader_cache_seed_image> seed_image;
    /// @brief Optional external expectations.  Zero means runtime must seal the
    /// selected Vulkan/Metal identities before vkCreateDevice; a nonzero value
    /// is an additional equality constraint, never a value to trust blindly.
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid{};
    std::uint64_t metal_device_registry_id = 0u;
    std::uint32_t qualification_wire_revision = 0u;
    std::uint64_t qualification_receipt_count = 0u;
    std::uint32_t helper_wire_revision = 0u;
    std::string helper_build_identity;
    std::string helper_source;
    std::uint64_t helper_executable_size = 0u;
    std::array<std::uint8_t, 32> helper_executable_sha256{};
    std::uint64_t helper_metal_registry_id = 0u;
    std::vector<runtime_metal_archive_spec> archives;

    [[nodiscard]] bool is_valid() const noexcept {
        if (!seed_image || !seed_image->is_accepted() || archives.empty()
            || qualification_wire_revision != 2u
            || qualification_receipt_count != archives.size()
            || helper_wire_revision != 2u
            || helper_build_identity != "bioseq_gpu_archive_builder"
            || helper_source != "installed-sibling"
            || helper_executable_size == 0u
            || helper_metal_registry_id == 0u
            || std::all_of(
                helper_executable_sha256.begin(),
                helper_executable_sha256.end(),
                [](std::uint8_t value) { return value == 0u; })
            || (metal_device_registry_id != 0u
                && metal_device_registry_id != helper_metal_registry_id)
            || !is_runtime_cache_digest_valid(
                seed_image->manifest().cache_digest)) {
            return false;
        }
        try {
        for (std::size_t i = 0u; i < archives.size(); ++i) {
            const runtime_metal_archive_spec& archive = archives[i];
            const sealed_shader_cache_artifact_image* image =
                seed_image->artifact(archive.artifact_ordinal);
            if (!archive.is_valid() || image == nullptr
                || image->descriptor.kind
                    != sealed_shader_cache_artifact_kind::metal_binary_archive
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
                    sealed_shader_cache_artifact_kind::
                        metal_binary_archive_qualification,
                    image->descriptor.key_index,
                    image->descriptor.filename + ".qualification");
            if (receipt_image == nullptr) return false;
            const auto receipt = sealed_shader_cache_detail::
                parse_metal_archive_qualification_receipt(
                    receipt_image->bytes);
            if (!receipt || !receipt->is_production_accepted()
                || receipt->archive_size != archive.size
                || receipt->archive_sha256 != archive.sha256
                || receipt->negative_error.domain
                    != archive.negative_error_domain
                || receipt->negative_error.code != archive.negative_error_code
                || receipt->wire_revision != qualification_wire_revision
                || receipt->helper_wire_revision
                    != helper_wire_revision
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
        const runtime_metal_archive_configuration&) const noexcept = default;
};

/// @brief Single admitted process owner for installed archive configuration metadata.
/// The public configuration is a transient installation candidate; its vector
/// and strings are moved into this image without a second allocation or copy.
class runtime_metal_archive_configuration_image {
public:
    runtime_metal_archive_configuration_image(
        const runtime_metal_archive_configuration_image&) = delete;
    runtime_metal_archive_configuration_image& operator=(
        const runtime_metal_archive_configuration_image&) = delete;
    runtime_metal_archive_configuration_image(
        runtime_metal_archive_configuration_image&&) noexcept = default;
    runtime_metal_archive_configuration_image& operator=(
        runtime_metal_archive_configuration_image&&) noexcept = delete;

    [[nodiscard]] const runtime_metal_archive_configuration& configuration()
        const noexcept {
        return configuration_;
    }
    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        return retained_capacity_bytes_;
    }
    [[nodiscard]] std::uint64_t peak_admitted_bytes() const noexcept {
        return peak_admitted_bytes_;
    }
    [[nodiscard]] bool is_valid() const noexcept {
        return configuration_.is_valid() && permit_
            && retained_capacity_bytes_ != 0u
            && retained_capacity_bytes_ <= peak_admitted_bytes_
            && permit_.bytes() == retained_capacity_bytes_;
    }

private:
    runtime_metal_archive_configuration_image(
        runtime_metal_archive_configuration configuration,
        std::uint64_t peak_admitted_bytes,
        bioseq::parallelism::memory_permit permit) noexcept
        : permit_(std::move(permit)),
          configuration_(std::move(configuration)),
          peak_admitted_bytes_(peak_admitted_bytes) {}

    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t owned = sizeof(runtime_metal_archive_configuration_image);
        owned = saturating_add(
            owned,
            saturating_mul(
                static_cast<std::uint64_t>(configuration_.archives.capacity()),
                sizeof(runtime_metal_archive_spec)));
        for (const runtime_metal_archive_spec& archive : configuration_.archives) {
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
    bioseq::parallelism::memory_permit permit_;
    runtime_metal_archive_configuration configuration_;
    std::uint64_t peak_admitted_bytes_ = 0u;
    std::uint64_t retained_capacity_bytes_ = 0u;

    friend std::expected<void, std::string>
    install_runtime_metal_archive_configuration(
        std::shared_ptr<const runtime_metal_archive_configuration_image>);
    friend std::expected<std::shared_ptr<
        const runtime_metal_archive_configuration_image>, std::string>
    make_runtime_metal_archive_configuration_image(
        std::shared_ptr<const sealed_shader_cache_seed_image>,
        std::array<std::uint8_t, VK_UUID_SIZE>,
        std::uint64_t);
};

/// @brief Counters and acceptance state for the immutable Metal archive live gate.
struct runtime_metal_archive_live_gate_result {
    std::uint64_t install_validation_count = 0u;
    std::uint64_t live_gate_calls = 0u;
    std::uint64_t live_configuration_scan_count = 0u;
    bool installed_accepted = false;
};

/// @brief Test-only report for private archive snapshot validation.
struct runtime_private_archive_snapshot_test_result {
    std::filesystem::path snapshot_path;
    std::uint64_t streamed_bytes = 0u;
    std::uint64_t scratch_capacity_bytes = 0u;
    bool exact_seed_compared = false;
    bool tamper_rejected = false;
};

/// @brief Captures the current immutable Metal archive live-gate counters.
/// @return The computed result.
[[nodiscard]] runtime_metal_archive_live_gate_result
capture_runtime_metal_archive_live_gate_result() noexcept;

/// @brief Executes the immutable Metal policy live gate in test builds.
void probe_immutable_metal_policy_live_for_test();

/// @brief Validates a private archive snapshot against supplied immutable bytes.
/// @return The computed result.
[[nodiscard]] std::expected<runtime_private_archive_snapshot_test_result, std::string>
verify_runtime_private_archive_snapshot_for_test(
    std::string_view artifact_filename,
    std::span<const std::byte> bytes,
    const std::filesystem::path& temp_parent);

/// @brief Stable aggregate identity of the exact ordered Metal archive set.
struct runtime_metal_archive_set_identity {
    std::uint64_t archive_count = 0u;
    std::uint64_t total_size = 0u;
    std::uint64_t ordered_checksum = 0u;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return archive_count != 0u && total_size != 0u
            && ordered_checksum != 0u;
    }
    [[nodiscard]] constexpr bool operator==(
        const runtime_metal_archive_set_identity&) const noexcept = default;
};

/// @brief Exact Vulkan device and pipeline-cache compatibility identity.
struct runtime_vulkan_device_identity {
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};

    [[nodiscard]] bool is_valid() const noexcept {
        return api_version != 0u && driver_version != 0u
            && std::any_of(
                device_uuid.begin(), device_uuid.end(),
                [](std::uint8_t value) { return value != 0u; })
            && std::any_of(
                pipeline_cache_uuid.begin(), pipeline_cache_uuid.end(),
                [](std::uint8_t value) { return value != 0u; });
    }
    [[nodiscard]] bool operator==(
        const runtime_vulkan_device_identity&) const noexcept = default;
};

/// @brief Immutable qualification issued for the process-resident Metal archive set.
struct runtime_metal_archive_process_qualification;
/// @brief Computes the stable identity of a process Metal archive qualification.
/// @return The computed result.
[[nodiscard]] std::uint64_t
runtime_metal_archive_process_qualification_identity(
    const runtime_metal_archive_process_qualification&) noexcept;

/// @brief Immutable process qualification created once while exact seed bytes are
/// installed into live Metal archive objects. Pipeline scopes borrow this
/// record; they never reopen or hash an archive pathname.
struct runtime_metal_archive_process_qualification {
    std::uint32_t wire_revision = 0u;
    std::uint64_t identity = 0u;
    runtime_metal_archive_set_identity archive_set{};
    std::uint64_t receipt_count = 0u;
    std::uint64_t hash_passes = 0u;
    std::uint64_t hashed_bytes = 0u;
    std::uint64_t private_files_created = 0u;
    std::uint64_t private_files_unlinked = 0u;
    std::uint64_t live_private_paths = 0u;
    std::uint64_t pipeline_filesystem_hash_passes = 0u;
    std::uint64_t pipeline_filesystem_hashed_bytes = 0u;
    std::uint32_t helper_wire_revision = 0u;
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

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return wire_revision == 2u && identity != 0u
            && identity
                == runtime_metal_archive_process_qualification_identity(*this)
            && archive_set.is_valid()
            && receipt_count == archive_set.archive_count
            && hash_passes == 1u && hashed_bytes == archive_set.total_size
            && private_files_created == archive_set.archive_count
            && private_files_unlinked == private_files_created
            && live_private_paths == 0u
            && pipeline_filesystem_hash_passes == 0u
            && pipeline_filesystem_hashed_bytes == 0u
            && helper_wire_revision == 2u
            && helper_build_identity == "bioseq_gpu_archive_builder"
            && helper_source == "installed-sibling"
            && helper_executable_size != 0u
            && std::any_of(
                helper_executable_sha256.begin(),
                helper_executable_sha256.end(),
                [](std::uint8_t value) { return value != 0u; })
            && helper_metal_registry_id != 0u
            && runtime_vulkan_device_identity{
                .device_uuid = vulkan_device_uuid,
                .api_version = api_version,
                .driver_version = driver_version,
                .vendor_id = vendor_id,
                .device_id = device_id,
                .pipeline_cache_uuid = pipeline_cache_uuid,
            }.is_valid()
            && seed_receipt_identity != 0u && seed_size != 0u
            && manifest_checksum != 0u
            && std::any_of(
                seed_sha256.begin(), seed_sha256.end(),
                [](std::uint8_t value) { return value != 0u; })
            && !saturated;
    }
    [[nodiscard]] bool operator==(
        const runtime_metal_archive_process_qualification&) const noexcept = default;
};

[[nodiscard]] inline std::uint64_t
runtime_metal_archive_process_qualification_identity(
    const runtime_metal_archive_process_qualification& value) noexcept
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
    append_u64(value.wire_revision);
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
    append_u64(value.helper_wire_revision);
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

[[nodiscard]] inline runtime_metal_archive_set_identity
make_runtime_metal_archive_set_identity(
    std::span<const runtime_metal_archive_spec> archives) noexcept {
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    std::uint64_t checksum = fnv_offset;
    std::uint64_t total_size = 0u;
    const auto append_byte = [&checksum](const std::uint8_t byte) {
        checksum ^= byte;
        checksum *= fnv_prime;
    };
    for (const runtime_metal_archive_spec& archive : archives) {
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
    return runtime_metal_archive_set_identity{
        .archive_count = static_cast<std::uint64_t>(archives.size()),
        .total_size = total_size,
        .ordered_checksum = checksum,
    };
}

/// @brief Exact facts captured for one vkCreateComputePipelines call.  A receipt is
/// accepted only when every Metal pipeline call made under its explicit scope
/// replaced the descriptor archive set, forced fail-on-miss, and first proved
/// that the same descriptor misses against a fresh empty archive.
struct runtime_metal_archive_pipeline_audit {
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
    runtime_metal_archive_process_qualification cache_qualification{};
    shader_variant_key shader_key{};
    std::string expected_entry_name;
    std::string observed_entry_name;
    std::uint64_t matched_descriptor_functions = 0u;
    runtime_metal_archive_set_identity expected_archives{};
    runtime_metal_archive_set_identity loaded_archives{};
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

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        const std::uint64_t actual_calls = actual_sync_calls + actual_async_calls;
        return receipt_identity != 0u && scope_identity != 0u
            && is_runtime_cache_digest_valid(cache_digest)
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
            && cache_qualification.is_internally_consistent()
            && cache_qualification.archive_set == expected_archives
            && cache_qualification.seed_receipt_identity
                == seed_receipt_identity
            && cache_qualification.seed_sha256 == seed_sha256
            && cache_qualification.seed_size == seed_size
            && cache_qualification.manifest_checksum == manifest_checksum
            && is_shader_variant_key_valid(shader_key)
            && !expected_entry_name.empty()
            && observed_entry_name == expected_entry_name
            && matched_descriptor_functions == 1u
            && expected_archives.is_valid()
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
/// @brief Transactional claim builder for runtime cache metrics.
class runtime_gpu_cache_metrics_claim_plan;
}

/// @brief Move-only immutable receipt retained by the canonical Vulkan pipeline cache
/// entry.  Cache views share that same object; they cannot mint another receipt.
class runtime_metal_archive_pipeline_receipt {
public:
    runtime_metal_archive_pipeline_receipt() = default;
    runtime_metal_archive_pipeline_receipt(
        const runtime_metal_archive_pipeline_receipt&) = delete;
    runtime_metal_archive_pipeline_receipt& operator=(
        const runtime_metal_archive_pipeline_receipt&) = delete;
    runtime_metal_archive_pipeline_receipt(
        runtime_metal_archive_pipeline_receipt&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          metrics_claimed_(std::exchange(
              other.metrics_claimed_, false)) {}
    runtime_metal_archive_pipeline_receipt& operator=(
        runtime_metal_archive_pipeline_receipt&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            metrics_claimed_ = std::exchange(
                other.metrics_claimed_, false);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return audit_.is_internally_consistent();
    }
    [[nodiscard]] const runtime_metal_archive_pipeline_audit& audit() const noexcept {
        return audit_;
    }

    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::common::saturating_add;
        std::uint64_t owned = sizeof(runtime_metal_archive_pipeline_receipt);
        owned = saturating_add(owned, audit_.cache_digest.capacity());
        owned = saturating_add(owned, audit_.expected_entry_name.capacity());
        owned = saturating_add(owned, audit_.observed_entry_name.capacity());
        owned = saturating_add(
            owned, audit_.expected_negative_error_domain.capacity());
        owned = saturating_add(
            owned, audit_.observed_negative_error_domain.capacity());
        return owned;
    }

    /// @brief Pure contract hook: proves move invalidation and that multiple cache
    /// views retain one accepted receipt object rather than cloning evidence.
    /// @return The computed result.
    [[nodiscard]] static bool self_test_move_and_reuse_contract(
        runtime_metal_archive_pipeline_audit audit) {
        runtime_metal_archive_pipeline_receipt issued{std::move(audit)};
        if (!issued.is_valid()) return false;
        runtime_metal_archive_pipeline_receipt moved{std::move(issued)};
        if (issued.is_valid() || !moved.is_valid()) return false;
        auto retained = std::make_shared<const runtime_metal_archive_pipeline_receipt>(
            std::move(moved));
        const auto normal_cache_view = retained;
        const auto compute_cache_view = retained;
        return retained->is_valid() && normal_cache_view->is_valid()
            && compute_cache_view->is_valid()
            && retained.get() == normal_cache_view.get()
            && retained.get() == compute_cache_view.get();
    }

private:
    explicit runtime_metal_archive_pipeline_receipt(
        runtime_metal_archive_pipeline_audit audit) noexcept
        : audit_(std::move(audit)) {}

    runtime_metal_archive_pipeline_audit audit_{};
    mutable bool metrics_claimed_ = false;
    friend class runtime;
    friend class runtime_pipeline_operation_token;
    friend class runtime_detail::runtime_gpu_cache_metrics_claim_plan;
};

static_assert(!std::is_copy_constructible_v<runtime_metal_archive_pipeline_receipt>);
static_assert(std::is_nothrow_move_constructible_v<
              runtime_metal_archive_pipeline_receipt>);

inline constexpr std::size_t k_runtime_pipeline_kind_slots = 64u;
inline constexpr std::size_t k_runtime_pipeline_prepared_slots = 11u;

/// @brief Exact 8-bit radix pass count for the persisted grouped-SoA Anchor ref_bits
/// domain: every target-coordinate bit plus the one canonical reverse-strand
/// bit. Kept public and allocation-free so portable contract tests can pin
/// every byte-width boundary used by causal dispatch validation.
/// @return The computed result.
[[nodiscard]] inline std::optional<std::uint32_t>
runtime_grouped_soa_radix_pass_count(
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
    return std::clamp<std::uint32_t>((bits + 8u) / 8u, 1u, 8u);
}

/// @brief Semantic operation families tracked by pipeline-use receipts.
enum class runtime_pipeline_operation_kind : std::uint8_t {
    invalid,
    query_sketch,
    grouped_soa_coordinate,
};

/// @brief Exact cache load and pipeline-creation metrics for one operation.
struct runtime_gpu_cache_metrics_audit {
    std::uint64_t token_identity = 0u;
    std::uint64_t gpu_pipeline_cache_loads = 0u;
    std::uint64_t gpu_pipeline_cache_seed_bytes = 0u;
    std::uint64_t gpu_pipeline_cache_load_ns = 0u;
    std::uint64_t gpu_pipeline_creates = 0u;
    std::uint64_t gpu_pipeline_creation_ns = 0u;
    bool saturated = false;

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
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
        const runtime_gpu_cache_metrics_audit&) const noexcept = default;
    [[nodiscard]] constexpr bool is_componentwise_bounded_by(
        const runtime_gpu_cache_metrics_audit& maximum) const noexcept {
        return is_internally_consistent()
            && maximum.is_internally_consistent()
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

/// @brief Fixed, nonallocating transaction prepared before the final immutable live
/// gate. Construction only observes report-once state. Destruction without
/// commit is a no-op, so a gate failure cannot consume seed/pipeline metrics.
/// runtime holds its queue/cache mutex across prepare, gate and commit.
class runtime_gpu_cache_metrics_claim_plan {
public:
    runtime_gpu_cache_metrics_claim_plan() = default;
    runtime_gpu_cache_metrics_claim_plan(
        const runtime_gpu_cache_metrics_claim_plan&) = delete;
    runtime_gpu_cache_metrics_claim_plan& operator=(
        const runtime_gpu_cache_metrics_claim_plan&) = delete;
    runtime_gpu_cache_metrics_claim_plan(
        runtime_gpu_cache_metrics_claim_plan&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          claims_(std::exchange(other.claims_, {})),
          claim_count_(std::exchange(other.claim_count_, 0u)),
          claim_seed_(std::exchange(other.claim_seed_, false)),
          valid_(std::exchange(other.valid_, false)),
          committed_(std::exchange(other.committed_, false)) {}
    runtime_gpu_cache_metrics_claim_plan& operator=(
        runtime_gpu_cache_metrics_claim_plan&& other) noexcept {
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

    [[nodiscard]] static runtime_gpu_cache_metrics_claim_plan prepare(
        std::uint64_t token_identity,
        const runtime_gpu_cache_metrics_audit& maximum,
        bool seed_already_claimed,
        std::span<const runtime_metal_archive_pipeline_receipt* const>
            receipts) noexcept {
        runtime_gpu_cache_metrics_claim_plan plan{};
        plan.audit_.token_identity = token_identity;
        if (token_identity == 0u || !maximum.is_internally_consistent()
            || maximum.token_identity != token_identity
            || receipts.empty() || receipts.size() > plan.claims_.size()) {
            return plan;
        }
        const runtime_metal_archive_pipeline_receipt* first = receipts.front();
        if (first == nullptr || !first->is_valid()) return plan;
        plan.claim_seed_ = !seed_already_claimed;
        if (plan.claim_seed_) {
            plan.audit_.gpu_pipeline_cache_loads = 1u;
            plan.audit_.gpu_pipeline_cache_seed_bytes =
                first->audit().seed_size;
            plan.audit_.gpu_pipeline_cache_load_ns =
                first->audit().seed_load_ns;
        }
        for (std::size_t i = 0u; i < receipts.size(); ++i) {
            const runtime_metal_archive_pipeline_receipt* receipt = receipts[i];
            if (receipt == nullptr || !receipt->is_valid()
                || receipt->audit().seed_size != first->audit().seed_size
                || receipt->audit().seed_load_ns
                    != first->audit().seed_load_ns) {
                return runtime_gpu_cache_metrics_claim_plan{};
            }
            for (std::size_t prior = 0u; prior < i; ++prior) {
                if (receipts[prior] == receipt) {
                    return runtime_gpu_cache_metrics_claim_plan{};
                }
            }
            if (receipt->metrics_claimed_) continue;
            if (plan.claim_count_ == plan.claims_.size()
                || plan.audit_.gpu_pipeline_creates
                    == (std::numeric_limits<std::uint64_t>::max)()
                || receipt->audit().pipeline_create_ns
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - plan.audit_.gpu_pipeline_creation_ns) {
                return runtime_gpu_cache_metrics_claim_plan{};
            }
            plan.claims_[plan.claim_count_++] = receipt;
            ++plan.audit_.gpu_pipeline_creates;
            plan.audit_.gpu_pipeline_creation_ns +=
                receipt->audit().pipeline_create_ns;
        }
        plan.valid_ = plan.audit_.is_componentwise_bounded_by(maximum);
        return plan;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return valid_ && audit_.is_internally_consistent();
    }
    [[nodiscard]] bool is_committed() const noexcept { return committed_; }
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& audit()
        const noexcept { return audit_; }

    void commit(bool& seed_claimed) noexcept {
        if (!is_valid() || committed_) return;
        if (claim_seed_) seed_claimed = true;
        for (std::size_t i = 0u; i < claim_count_; ++i) {
            claims_[i]->metrics_claimed_ = true;
        }
        committed_ = true;
    }

private:
    runtime_gpu_cache_metrics_audit audit_{};
    std::array<const runtime_metal_archive_pipeline_receipt*,
               k_runtime_pipeline_prepared_slots> claims_{};
    std::size_t claim_count_ = 0u;
    bool claim_seed_ = false;
    bool valid_ = false;
    bool committed_ = false;
};

static_assert(!std::is_copy_constructible_v<
    runtime_gpu_cache_metrics_claim_plan>);
static_assert(std::is_nothrow_move_constructible_v<
    runtime_gpu_cache_metrics_claim_plan>);

}  // namespace runtime_detail

/// @brief Move-only metrics delta that may be consumed exactly once.
class runtime_gpu_cache_metrics_delta {
public:
    runtime_gpu_cache_metrics_delta() = default;
    runtime_gpu_cache_metrics_delta(const runtime_gpu_cache_metrics_delta&) = delete;
    runtime_gpu_cache_metrics_delta& operator=(
        const runtime_gpu_cache_metrics_delta&) = delete;
    runtime_gpu_cache_metrics_delta(
        runtime_gpu_cache_metrics_delta&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          issued_(std::exchange(other.issued_, false)) {}
    runtime_gpu_cache_metrics_delta& operator=(
        runtime_gpu_cache_metrics_delta&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            issued_ = std::exchange(other.issued_, false);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && audit_.is_internally_consistent();
    }
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& audit() const noexcept {
        return audit_;
    }
    void consume() noexcept {
        DEBUG_ASSERT(is_valid(),
            "persisted GPU cache metrics token was not prevalidated");
        audit_ = {};
        issued_ = false;
    }

private:
    runtime_gpu_cache_metrics_audit audit_{};
    bool issued_ = false;
    friend class runtime;
};

static_assert(!std::is_copy_constructible_v<runtime_gpu_cache_metrics_delta>);
static_assert(std::is_nothrow_move_constructible_v<
    runtime_gpu_cache_metrics_delta>);

/// @brief Accepted immutable pipeline-cache policy for GPU operations.
enum class runtime_pipeline_cache_policy : std::uint8_t {
    invalid,
    immutable_preseed_all_hit,
};

/// @brief Immutable GPU environment identity attached to accepted operations.
struct runtime_gpu_execution_environment_snapshot {
    bool gpu_environment_present = false;
    std::array<std::uint8_t, VK_UUID_SIZE> gpu_device_uuid{};
    std::array<char, 128> gpu_driver_identity{};
    runtime_pipeline_cache_policy cache_policy =
        runtime_pipeline_cache_policy::invalid;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    bool pipeline_cache_all_hit = false;

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return gpu_environment_present
            && std::any_of(
                gpu_device_uuid.begin(), gpu_device_uuid.end(),
                [](std::uint8_t value) { return value != 0u; })
            && std::any_of(
                gpu_driver_identity.begin(), gpu_driver_identity.end(),
                [](char value) { return value != '\0'; })
            && cache_policy
                == runtime_pipeline_cache_policy::
                    immutable_preseed_all_hit
            && std::any_of(
                seed_sha256.begin(), seed_sha256.end(),
                [](std::uint8_t value) { return value != 0u; })
            && seed_size != 0u && manifest_checksum != 0u
            && pipeline_cache_all_hit;
    }
    [[nodiscard]] constexpr bool operator==(
        const runtime_gpu_execution_environment_snapshot&) const noexcept = default;
};

/// @brief Preparation receipt for one shader pipeline identity.
struct runtime_pipeline_prepared_audit {
    shader_variant_key shader_key{};
    std::uint64_t receipt_identity = 0u;
    std::uint64_t prepared_calls = 0u;
};

/// @brief Per-job dispatch, workgroup, and submission accounting.
struct runtime_pipeline_job_use_audit {
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

    [[nodiscard]] std::uint64_t count(shader_kind kind) const noexcept {
        const std::size_t slot = static_cast<std::size_t>(kind);
        return slot < dispatches.size() ? dispatches[slot] : 0u;
    }
};

/// @brief Complete causal audit for one runtime pipeline operation.
struct runtime_pipeline_operation_audit {
    runtime_pipeline_operation_kind kind = runtime_pipeline_operation_kind::invalid;
    std::uint64_t operation_identity = 0u;
    std::array<runtime_pipeline_prepared_audit,
               k_runtime_pipeline_prepared_slots> prepared{};
    std::size_t prepared_count = 0u;
    std::vector<runtime_pipeline_job_use_audit> jobs{};
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
    runtime_metal_archive_set_identity archive_set{};
    runtime_metal_archive_process_qualification cache_qualification{};
    runtime_gpu_cache_metrics_audit cache_metrics{};
};

/// @brief Collects pipeline preparation and committed dispatch use for one operation.
class runtime_pipeline_use_collector;
/// @brief Move-only receipt for a completed grouped-SoA coordinate operation.
class persisted_grouped_soa_coordinate_operation_receipt;

/// @brief Move-only causal receipt that binds PREPARED cache receipts to dispatches
/// committed only after submit, fence wait, immutable live gate, and reset.
class runtime_pipeline_operation_token {
public:
    runtime_pipeline_operation_token() = default;
    runtime_pipeline_operation_token(const runtime_pipeline_operation_token&) =
        delete;
    runtime_pipeline_operation_token& operator=(
        const runtime_pipeline_operation_token&) = delete;
    runtime_pipeline_operation_token(
        runtime_pipeline_operation_token&& other) noexcept
        : capacity_permit_(std::move(other.capacity_permit_)),
          audit_(std::move(other.audit_)),
          receipts_(std::exchange(other.receipts_, {})),
          issued_(std::exchange(other.issued_, false)) {
        other.audit_ = {};
    }
    runtime_pipeline_operation_token& operator=(
        runtime_pipeline_operation_token&& other) noexcept {
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

    [[nodiscard]] const runtime_pipeline_operation_audit& audit()
        const noexcept { return audit_; }
    [[nodiscard]] bool is_valid_for_query_sketch() const noexcept;
    [[nodiscard]] bool is_valid_for_grouped_soa_coordinate() const noexcept;
    [[nodiscard]] static bool self_test_causal_collector_contract(
        runtime_metal_archive_pipeline_audit base_audit);

private:
    [[nodiscard]] bool is_common_state_valid() const noexcept;
    [[nodiscard]] std::uint64_t prepared_calls(
        shader_kind kind) const noexcept;
    [[nodiscard]] bool is_prepared_exactly(
        std::span<const shader_kind> required,
        std::uint64_t radix_prepared_calls = 0u) const noexcept;

    // Declared before the vector owner so destruction deallocates jobs before
    // releasing their reservation (members are destroyed in reverse order).
    bioseq::parallelism::memory_permit capacity_permit_{};
    runtime_pipeline_operation_audit audit_{};
    std::array<const runtime_metal_archive_pipeline_receipt*,
               k_runtime_pipeline_prepared_slots> receipts_{};
    bool issued_ = false;
    friend class runtime;
    friend class runtime_pipeline_use_collector;
    friend class persisted_grouped_soa_coordinate_operation_receipt;
};

static_assert(!std::is_copy_constructible_v<runtime_pipeline_operation_token>);
static_assert(std::is_nothrow_move_constructible_v<
    runtime_pipeline_operation_token>);

inline bool runtime_pipeline_operation_token::is_common_state_valid() const noexcept {
    if (!issued_ || audit_.operation_identity == 0u
        || audit_.kind == runtime_pipeline_operation_kind::invalid
        || audit_.jobs.empty() || audit_.prepared_count == 0u
        || audit_.prepared_count > audit_.prepared.size()
        || audit_.committed_submissions == 0u
        || audit_.committed_command_generations
            != audit_.committed_submissions
        || audit_.begun_command_generations
            != audit_.committed_command_generations
        || audit_.burned_pending_uses != 0u || audit_.pending
        || audit_.invalid || audit_.saturated
        || !audit_.cache_metrics.is_internally_consistent()
        || audit_.cache_metrics.token_identity != audit_.operation_identity
        || audit_.api_version == 0u || audit_.driver_version == 0u
        || audit_.seed_size == 0u || audit_.manifest_checksum == 0u
        || audit_.seed_receipt_identity == 0u
        || !audit_.archive_set.is_valid()
        || !audit_.cache_qualification.is_internally_consistent()
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
        const runtime_pipeline_prepared_audit& prepared = audit_.prepared[i];
        const runtime_metal_archive_pipeline_receipt* receipt = receipts_[i];
        if (receipt == nullptr || !receipt->is_valid()
            || prepared.prepared_calls == 0u
            || prepared.receipt_identity == 0u
            || !is_shader_variant_key_valid(prepared.shader_key)) {
            return false;
        }
        const runtime_metal_archive_pipeline_audit& pipeline = receipt->audit();
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
    for (const runtime_pipeline_job_use_audit& job : audit_.jobs) {
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

inline std::uint64_t runtime_pipeline_operation_token::prepared_calls(
    shader_kind kind) const noexcept {
    const std::uint32_t raw = static_cast<std::uint32_t>(kind);
    for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
        if (audit_.prepared[i].shader_key.kind == raw) {
            return audit_.prepared[i].prepared_calls;
        }
    }
    return 0u;
}

inline bool runtime_pipeline_operation_token::is_prepared_exactly(
    std::span<const shader_kind> required,
    std::uint64_t radix_prepared_calls) const noexcept {
    const std::size_t expected_count = required.size()
        + (radix_prepared_calls == 0u ? 0u : 3u);
    if (audit_.prepared_count != expected_count) return false;
    for (const shader_kind kind : required) {
        if (prepared_calls(kind) != 1u) return false;
    }
    const std::array radix{
        shader_kind::radix_sort_histogram,
        shader_kind::radix_sort_scan,
        shader_kind::radix_sort_scatter,
    };
    for (const shader_kind kind : radix) {
        if (prepared_calls(kind) != radix_prepared_calls) return false;
    }
    for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
        const shader_kind actual = static_cast<shader_kind>(
            audit_.prepared[i].shader_key.kind);
        const bool base = std::find(required.begin(), required.end(), actual)
            != required.end();
        const bool sort = std::find(radix.begin(), radix.end(), actual)
            != radix.end();
        if (!base && !(radix_prepared_calls != 0u && sort)) return false;
    }
    return true;
}

inline bool runtime_pipeline_operation_token::is_valid_for_query_sketch()
    const noexcept {
    constexpr std::array required{
        shader_kind::ordered_sketch_tile,
        shader_kind::prefix_scan_block,
        shader_kind::prefix_scan_add,
    };
    if (!is_common_state_valid()
        || audit_.kind != runtime_pipeline_operation_kind::query_sketch
        || audit_.jobs.size() != 1u || audit_.committed_submissions != 2u
        || !is_prepared_exactly(required)) {
        return false;
    }
    const runtime_pipeline_job_use_audit& job = audit_.jobs.front();
    if (job.committed_submissions != 2u
        || job.count(shader_kind::ordered_sketch_tile) != 2u
        || job.count(shader_kind::prefix_scan_block) != 1u
        || job.count(shader_kind::prefix_scan_add) != 2u) {
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

inline bool runtime_pipeline_operation_token::is_valid_for_grouped_soa_coordinate()
    const noexcept {
    constexpr std::array required{
        shader_kind::grouped_soa_seed_count,
        shader_kind::grouped_soa_seed_emit,
        shader_kind::prefix_scan_block,
        shader_kind::prefix_scan_add,
        shader_kind::chain_range_build,
        shader_kind::coordinate_reduce,
        shader_kind::chain_tiled_dp_u64,
    };
    if (!is_common_state_valid()
        || audit_.kind
            != runtime_pipeline_operation_kind::grouped_soa_coordinate) {
        return false;
    }
    std::uint64_t anchored_jobs = 0u;
    for (const runtime_pipeline_job_use_audit& job : audit_.jobs) {
        if (!job.shape_contract_set || job.minimizer_count == 0u
            || job.target_len == 0u
            || job.target_position_offset
                > (std::numeric_limits<std::uint64_t>::max)()
                    - (job.target_len - 1u)) {
            return false;
        }
        const auto pass_count = runtime_grouped_soa_radix_pass_count(
            job.target_position_offset, job.target_len);
        if (!pass_count.has_value()) return false;
        const std::uint64_t passes = *pass_count;
        const bool front =
            job.count(shader_kind::grouped_soa_seed_count) == 1u
            && job.count(shader_kind::prefix_scan_block) == 1u
            && job.count(shader_kind::prefix_scan_add) == 2u;
        if (!front) return false;
        std::uint64_t expected_total = 4u;
        std::uint64_t expected_submissions = 1u;
        if (job.anchor_count == 0u) {
            if (job.range_count != 0u) return false;
        } else {
            ++anchored_jobs;
            expected_total = 6u + 3u * passes;
            expected_submissions = 3u;
            if (job.count(shader_kind::grouped_soa_seed_emit) != 1u
                || job.count(shader_kind::radix_sort_histogram)
                    != passes
                || job.count(shader_kind::radix_sort_scan) != passes
                || job.count(shader_kind::radix_sort_scatter)
                    != passes
                || job.count(shader_kind::chain_range_build) != 1u) {
                return false;
            }
            if (job.range_count != 0u) {
                expected_total += 2u;
                expected_submissions = 4u;
                if (job.count(shader_kind::chain_tiled_dp_u64) != 1u
                    || job.count(shader_kind::coordinate_reduce)
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
            || job.count(shader_kind::coordinate_vote_reduce) != 0u
            || job.count(
                shader_kind::coordinate_vote_reduce_batch) != 0u
            || job.count(shader_kind::ordered_sketch_tile) != 0u) {
            return false;
        }
    }
    return is_prepared_exactly(required, anchored_jobs);
}

/// @brief Mutable collector used while recording and committing GPU submissions.
class runtime_pipeline_use_collector {
public:
    runtime_pipeline_use_collector(const runtime_pipeline_use_collector&) = delete;
    runtime_pipeline_use_collector& operator=(
        const runtime_pipeline_use_collector&) = delete;
    runtime_pipeline_use_collector(runtime_pipeline_use_collector&&) = delete;
    runtime_pipeline_use_collector& operator=(
        runtime_pipeline_use_collector&&) = delete;
    ~runtime_pipeline_use_collector() { burn_pending(); }

private:
    struct pending_job {
        std::array<std::uint64_t,
                   k_runtime_pipeline_kind_slots> dispatches{};
        std::array<std::uint64_t,
                   k_runtime_pipeline_kind_slots> workgroups{};
    };

    [[nodiscard]] static std::unique_ptr<runtime_pipeline_use_collector> create(
        runtime_pipeline_operation_kind kind,
        std::size_t job_count,
        std::uint64_t operation_identity) {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (kind == runtime_pipeline_operation_kind::invalid
            || job_count == 0u || operation_identity == 0u
            || job_count
                > std::vector<runtime_pipeline_job_use_audit>{}.max_size()
            || job_count > std::vector<pending_job>{}.max_size()) {
            throw vulkan_runtime_error(
                "runtime pipeline receipt collector shape is invalid");
        }
        std::uint64_t retained = sizeof(runtime_pipeline_use_collector);
        retained = saturating_add(retained, 2u * 1024u);
        retained = saturating_add(
            retained,
            saturating_mul(
                static_cast<std::uint64_t>(job_count),
                sizeof(runtime_pipeline_job_use_audit)));
        retained = saturating_add(
            retained,
            saturating_mul(
                static_cast<std::uint64_t>(job_count),
                sizeof(pending_job)));
        const std::uint64_t peak = saturating_mul(
            retained,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw vulkan_runtime_error(
                "runtime pipeline receipt collector admission saturated");
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(peak, &failure);
        if (!permit) {
            throw vulkan_runtime_error(shader_cache_message(
                "runtime pipeline receipt collector admission denied: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(
                    failure.reason),
                " requested=", failure.requested_bytes));
        }
        std::unique_ptr<runtime_pipeline_use_collector> result;
        try {
            result.reset(new runtime_pipeline_use_collector(
                kind, job_count, operation_identity, std::move(*permit)));
        } catch (const std::bad_alloc&) {
            throw vulkan_runtime_error(
                "runtime pipeline receipt collector allocation failed after admission");
        }
        result->reconcile_capacity_permit();
        return result;
    }

    runtime_pipeline_use_collector(
        runtime_pipeline_operation_kind kind,
        std::size_t job_count,
        std::uint64_t operation_identity,
        bioseq::parallelism::memory_permit capacity_permit)
        : capacity_permit_(std::move(capacity_permit)),
          pending_(job_count) {
        audit_.kind = kind;
        audit_.operation_identity = operation_identity;
        audit_.jobs.resize(job_count);
        audit_.cache_metrics.token_identity = operation_identity;
        submission_ordinal_cursor_ = 0u;
    }

    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t retained = sizeof(runtime_pipeline_use_collector);
        retained = saturating_add(retained, 2u * 1024u);
        retained = saturating_add(
            retained,
            saturating_mul(
                static_cast<std::uint64_t>(audit_.jobs.capacity()),
                sizeof(runtime_pipeline_job_use_audit)));
        retained = saturating_add(
            retained,
            saturating_mul(
                static_cast<std::uint64_t>(pending_.capacity()),
                sizeof(pending_job)));
        return retained;
    }

    void reconcile_capacity_permit() {
        const std::uint64_t retained = retained_capacity_bytes();
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || !capacity_permit_
            || retained > capacity_permit_.bytes()) {
            throw vulkan_runtime_error(
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
        const shader_variant_key& key,
        const runtime_metal_archive_pipeline_receipt* receipt) noexcept {
        if (audit_.invalid || receipt == nullptr || !receipt->is_valid()
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
            audit_.prepared[slot] = runtime_pipeline_prepared_audit{
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

        const runtime_metal_archive_pipeline_audit& source = receipt->audit();
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
        // under runtime::mu_ after the immutable live gate succeeds.
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
        runtime_pipeline_job_use_audit& job = audit_.jobs[job_index];
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
        const shader_variant_key& key,
        const runtime_metal_archive_pipeline_receipt* receipt,
        std::uint32_t group_count,
        std::uint64_t observed_generation = 0u) noexcept {
        if (!active_ || audit_.invalid || job_index >= pending_.size()
            || vulkan_handle_identity(command_buffer) != command_identity_
            || group_count == 0u
            || (observed_generation != 0u
                && observed_generation != active_generation_)
            || receipt == nullptr || !receipt->is_valid()) {
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
                runtime_pipeline_job_use_audit& job = audit_.jobs[job_index];
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
    [[nodiscard]] runtime_pipeline_operation_token seal() noexcept {
        if (active_) {
            audit_.pending = true;
            burn_pending();
            invalidate();
        }
        runtime_pipeline_operation_token receipt{};
        receipt.audit_ = std::move(audit_);
        receipt.receipts_ = receipts_;
        receipt.issued_ = true;
        // Pending storage no longer contributes to the sealed receipt. Destroy
        // it before shrinking/transferring the permit. Keep the collector
        // object itself covered until its unique_ptr owner destroys it; the
        // resulting small over-reservation then travels with the jobs vector
        // for the complete lifetime of the move-only receipt.
        {
            std::vector<pending_job> empty;
            pending_.swap(empty);
        }
        {
            // The standard allocator transfers this capacity, but explicitly
            // clear any valid moved-from residue before the permit leaves the
            // collector so the ownership receipt is allocator-independent.
            std::vector<runtime_pipeline_job_use_audit> empty;
            audit_.jobs.swap(empty);
        }
        const std::uint64_t retained_jobs =
            bioseq::common::saturating_add(
                sizeof(runtime_pipeline_use_collector),
                bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(receipt.audit_.jobs.capacity()),
                sizeof(runtime_pipeline_job_use_audit)));
        if (retained_jobs
                == (std::numeric_limits<std::uint64_t>::max)()
            || retained_jobs > capacity_permit_.bytes()) {
            receipt.audit_.invalid = true;
        } else {
            capacity_permit_.shrink_to(retained_jobs);
        }
        receipt.capacity_permit_ = std::move(capacity_permit_);
        receipts_.fill(nullptr);
        audit_ = {};
        return receipt;
    }

    // Kept before both vector owners: they release capacity before the permit.
    bioseq::parallelism::memory_permit capacity_permit_{};
    runtime_pipeline_operation_audit audit_{};
    std::array<const runtime_metal_archive_pipeline_receipt*,
               k_runtime_pipeline_prepared_slots> receipts_{};
    std::vector<pending_job> pending_{};
    std::uint64_t command_identity_ = 0u;
    std::uint64_t active_generation_ = 0u;
    std::uint64_t submission_ordinal_cursor_ = 0u;
    bool environment_sealed_ = false;
    bool active_ = false;

    friend class runtime;
    friend class runtime_pipeline_operation_token;
};

inline bool runtime_pipeline_operation_token::
self_test_causal_collector_contract(
    runtime_metal_archive_pipeline_audit base_audit) {
    if (!base_audit.is_internally_consistent() || base_audit.pipeline_create_ns == 0u) {
        return false;
    }
    const std::array<shader_variant_key, 11> keys{
        make_shader_key(
            shader_kind::ordered_sketch_tile, 384u, 64u, 10u),
        make_shader_key(
            shader_kind::prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::prefix_scan_add, 384u, 128u, 1u),
        make_shader_key(
            shader_kind::grouped_soa_seed_count, 384u, 128u, 3u),
        make_shader_key(
            shader_kind::grouped_soa_seed_emit, 384u, 128u, 3u),
        make_shader_key(
            shader_kind::chain_range_build, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::coordinate_reduce, 384u, 128u, 1u),
        make_shader_key(
            shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u),
        make_shader_key(
            shader_kind::radix_sort_histogram, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::radix_sort_scan, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::radix_sort_scatter, 384u, 1u, 1u),
    };
    if (!std::all_of(
            keys.begin(), keys.end(),
            [](const shader_variant_key& key) {
                return is_shader_variant_key_valid(key);
            })) {
        return false;
    }
    std::array<runtime_metal_archive_pipeline_receipt, 11> receipts{};
    bool seed_claimed = false;
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        runtime_metal_archive_pipeline_audit audit = base_audit;
        audit.receipt_identity = 1000u + i;
        audit.scope_identity = 2000u + i;
        audit.shader_key = keys[i];
        receipts[i] = runtime_metal_archive_pipeline_receipt{
            std::move(audit)};
        if (!receipts[i].is_valid()) return false;
    }
    const auto command = [](std::uintptr_t identity) {
        return reinterpret_cast<VkCommandBuffer>(identity);
    };
    const auto prepare_query = [&keys, &receipts](
        runtime_pipeline_use_collector& collector) {
        for (std::size_t i = 0u; i < 3u; ++i) {
            collector.prepare(keys[i], &receipts[i]);
        }
    };
    const auto query_golden = [&]() {
        auto collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::query_sketch, 1u, 101u);
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

    runtime_pipeline_operation_token query{};
    try {
        query = query_golden();
    } catch (...) {
        return false;
    }
    if (!query.is_valid_for_query_sketch()
        || query.audit_.begun_command_generations != 2u
        || query.audit_.committed_command_generations != 2u
        || query.audit_.jobs[0].submission_operation_identity != 101u
        || query.audit_.jobs[0].submission_ordinals[0] != 1u
        || query.audit_.jobs[0].submission_ordinals[1] != 2u
        || query.audit_.jobs[0].count(
            shader_kind::ordered_sketch_tile) != 2u
        || query.audit_.jobs[0].workgroups[static_cast<std::size_t>(
            shader_kind::ordered_sketch_tile)] != 6u
        || query.audit_.jobs[0].workgroups[static_cast<std::size_t>(
            shader_kind::prefix_scan_block)] != 2u
        || query.audit_.jobs[0].workgroups[static_cast<std::size_t>(
            shader_kind::prefix_scan_add)] != 3u
        || !query.capacity_permit_) {
        return false;
    }
    runtime_pipeline_operation_token moved_query{std::move(query)};
    if (query.is_valid_for_query_sketch() || query.capacity_permit_
        || !moved_query.is_valid_for_query_sketch()
        || !moved_query.capacity_permit_) {
        return false;
    }
    query = std::move(moved_query);
    if (moved_query.is_valid_for_query_sketch()
        || moved_query.capacity_permit_
        || !query.is_valid_for_query_sketch()) {
        return false;
    }

    const auto rejected_query = [&](std::uint64_t identity, int mode) {
        auto collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::query_sketch, 1u, identity);
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
        const runtime_pipeline_operation_token reset = rejected_query(102u, 0);
        const runtime_pipeline_operation_token empty = rejected_query(103u, 1);
        const runtime_pipeline_operation_token stale = rejected_query(104u, 2);
        const runtime_pipeline_operation_token uncommitted =
            rejected_query(105u, 3);
        const runtime_pipeline_operation_token wrong_command =
            rejected_query(106u, 4);
        const runtime_pipeline_operation_token wrong_receipt =
            rejected_query(107u, 5);
        const runtime_pipeline_operation_token zero_groups =
            rejected_query(108u, 6);
        if (reset.is_valid_for_query_sketch()
            || reset.audit_.burned_pending_uses != 1u
            || empty.is_valid_for_query_sketch()
            || empty.audit_.begun_command_generations != 1u
            || stale.is_valid_for_query_sketch()
            || uncommitted.is_valid_for_query_sketch()
            || wrong_command.is_valid_for_query_sketch()
            || wrong_receipt.is_valid_for_query_sketch()
            || zero_groups.is_valid_for_query_sketch()) {
            return false;
        }
    } catch (...) {
        return false;
    }

    runtime_pipeline_operation_token shape{};
    try {
        auto collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::grouped_soa_coordinate, 3u, 202u);
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
            collector->use(command(13u), 1u, keys[slot], &receipts[slot], 1u);
            collector->use(command(13u), 2u, keys[slot], &receipts[slot], 1u);
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
    if (!shape.is_valid_for_grouped_soa_coordinate()
        || shape.audit_.begun_command_generations != 4u
        || shape.audit_.committed_command_generations != 4u
        || !shape.capacity_permit_) {
        return false;
    }
    constexpr std::array<std::uint64_t, 3> expected_submissions{1u, 3u, 4u};
    for (std::size_t job = 0u; job < shape.audit_.jobs.size(); ++job) {
        const runtime_pipeline_job_use_audit& audit = shape.audit_.jobs[job];
        if (audit.submission_operation_identity != 202u
            || audit.committed_submissions != expected_submissions[job]
            || audit.count(
                shader_kind::coordinate_vote_reduce) != 0u
            || audit.count(
                shader_kind::coordinate_vote_reduce_batch) != 0u) {
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
    runtime_gpu_cache_metrics_audit maximum{
        .token_identity = 303u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = receipts[0].audit().seed_size,
        .gpu_pipeline_cache_load_ns = receipts[0].audit().seed_load_ns,
        .gpu_pipeline_creates =
            static_cast<std::uint64_t>(receipts.size()),
    };
    for (const runtime_metal_archive_pipeline_receipt& receipt : receipts) {
        if (receipt.audit().pipeline_create_ns
            > (std::numeric_limits<std::uint64_t>::max)()
                - maximum.gpu_pipeline_creation_ns) {
            return false;
        }
        maximum.gpu_pipeline_creation_ns +=
            receipt.audit().pipeline_create_ns;
    }
    std::array<const runtime_metal_archive_pipeline_receipt*, 11>
        receipt_pointers{};
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        receipt_pointers[i] = &receipts[i];
    }
    const auto pointers =
        std::span<const runtime_metal_archive_pipeline_receipt* const>{
            receipt_pointers.data(), receipt_pointers.size()};
    {
        auto abandoned = runtime_detail::
            runtime_gpu_cache_metrics_claim_plan::prepare(
                maximum.token_identity, maximum, seed_claimed, pointers);
        if (!abandoned.is_valid()
            || abandoned.audit() != maximum
            || abandoned.is_committed()) {
            return false;
        }
    }
    if (seed_claimed
        || std::any_of(
            receipts.begin(), receipts.end(),
            [](const auto& receipt) { return receipt.metrics_claimed_; })) {
        return false;
    }
    auto first = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        maximum.token_identity, maximum, seed_claimed, pointers);
    if (!first.is_valid() || first.audit() != maximum) return false;
    first.commit(seed_claimed);
    if (!first.is_committed() || !seed_claimed
        || std::any_of(
            receipts.begin(), receipts.end(),
            [](const auto& receipt) { return !receipt.metrics_claimed_; })) {
        return false;
    }
    runtime_gpu_cache_metrics_audit later_maximum = maximum;
    later_maximum.token_identity = 304u;
    auto later = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        later_maximum.token_identity, later_maximum,
        seed_claimed, pointers);
    if (!later.is_valid()
        || later.audit().token_identity != later_maximum.token_identity
        || later.audit().gpu_pipeline_cache_loads != 0u
        || later.audit().gpu_pipeline_cache_seed_bytes != 0u
        || later.audit().gpu_pipeline_cache_load_ns != 0u
        || later.audit().gpu_pipeline_creates != 0u
        || later.audit().gpu_pipeline_creation_ns != 0u) {
        return false;
    }
    later.commit(seed_claimed);
    return later.is_committed() && seed_claimed;
}

/// @brief Install-once production seam.  The supervisor calls this from its runtime
/// prepare hook, before the first runtime singleton construction.
/// @return The computed result.
[[nodiscard]] std::expected<void, std::string>
install_runtime_metal_archive_configuration(
    std::shared_ptr<const runtime_metal_archive_configuration_image> configuration);

[[nodiscard]] std::expected<std::shared_ptr<
    const runtime_metal_archive_configuration_image>, std::string>
make_runtime_metal_archive_configuration_image(
    std::shared_ptr<const sealed_shader_cache_seed_image> seed_image,
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid = {},
    std::uint64_t metal_device_registry_id = 0u);

[[nodiscard]] bool is_runtime_metal_archive_configuration_installed() noexcept;

/// @brief The 36-byte push-constant block consumed by the tiled-DP shader. Layout is
/// pinned to the GLSL `Params` block.
struct chaining_push_constants {
    std::uint32_t anchor_count;
    std::uint32_t range_count;
    std::int32_t max_dist_x;
    std::int32_t max_dist_y;
    std::int32_t bandwidth;
    std::int32_t max_iter;
    float gap_penalty;
    float skip_penalty;
    std::int32_t max_chain_skip;  // : nearest-first early-exit (minimap2 max_skip); <=0 = disabled
};
static_assert(sizeof(chaining_push_constants) == 36);
static_assert(offsetof(chaining_push_constants, anchor_count) == 0);
static_assert(offsetof(chaining_push_constants, range_count) == 4);
static_assert(offsetof(chaining_push_constants, max_dist_x) == 8);
static_assert(offsetof(chaining_push_constants, max_dist_y) == 12);
static_assert(offsetof(chaining_push_constants, bandwidth) == 16);
static_assert(offsetof(chaining_push_constants, max_iter) == 20);
static_assert(offsetof(chaining_push_constants, gap_penalty) == 24);
static_assert(offsetof(chaining_push_constants, skip_penalty) == 28);
static_assert(offsetof(chaining_push_constants, max_chain_skip) == 32);

/// @brief Push-constant block for the  packed sequence generator kernel.
struct generate_units_push {
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
static_assert(sizeof(generate_units_push) == 48);
static_assert(std::is_standard_layout_v<generate_units_push>);

/// @brief Result of one complete minimizer sketch roundtrip. The
/// resident `minimizer_words` device buffer is moved into the caller's
/// `vulkan_device_minimizer_storage` for seed-lookup reuse.  Host materialisation
/// is deliberately deferred to the caller, after the arena and packed-sequence
/// transients have been released and the exact `count` is known.
struct runtime_sketch_result {
    buffer minimizer_words;
    std::uint64_t count = 0;
    // Operation-originated lifetime receipt.  A successful roundtrip may hand
    // its standalone output buffer to the caller only after every shared-arena
    // backing buffer has been destroyed.  The caller consumes this receipt
    // before shrinking dispatch admission or reserving exact host storage.
    bool arena_trimmed_before_return = false;
    bool overflow = false;
};

/// @brief Whether a completed Vulkan sketch must also materialize its packed hashes
/// in a host vector.  Device-only consumers still receive the exact count and
/// resident device buffer; they do not pay for a hash download/decode.
enum class vulkan_sketch_host_materialization : std::uint8_t {
    host_and_device,
    device_only,
};

/// @brief Operation-originated evidence for both batch sketch protocols.  The
/// established host-materialized path records count/scan/emit in one submit;
/// device-only mode separates emit so its output can use the exact scanned
/// count.  Default construction is deliberately invalid.
class runtime_sketch_dispatch_stats {
public:
    runtime_sketch_dispatch_stats() = default;
    runtime_sketch_dispatch_stats(
        const runtime_sketch_dispatch_stats&) = default;
    runtime_sketch_dispatch_stats& operator=(
        const runtime_sketch_dispatch_stats&) = default;

    [[nodiscard]] constexpr vulkan_sketch_host_materialization
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
    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        const bool dispatch_shape_accepted = count_dispatches_ == 1u
            && prefix_scan_sequences_ == 1u
            && prefix_scan_dispatches_ == 3u
            && emit_dispatches_ == 1u;
        if (!dispatch_shape_accepted) return false;
        switch (materialization_) {
            case vulkan_sketch_host_materialization::host_and_device:
                return completed_roundtrips_ == 1u;
            case vulkan_sketch_host_materialization::device_only:
                return completed_roundtrips_ == 2u;
        }
        return false;
    }

private:
    friend class runtime;

    vulkan_sketch_host_materialization materialization_ =
        vulkan_sketch_host_materialization::host_and_device;
    std::uint64_t count_dispatches_ = 0;
    std::uint64_t prefix_scan_sequences_ = 0;
    std::uint64_t prefix_scan_dispatches_ = 0;
    std::uint64_t emit_dispatches_ = 0;
    std::uint64_t completed_roundtrips_ = 0;
};

static_assert(!runtime_sketch_dispatch_stats{}.is_internally_consistent());

/// @brief Exact per-query partition of one batched query-minimizer image.
struct gpu_seed_query_record_wire {
    std::uint32_t minimizer_offset = 0;
    std::uint32_t minimizer_count = 0;
    std::uint32_t query_len = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(gpu_seed_query_record_wire) == 16);
static_assert(alignof(gpu_seed_query_record_wire) == alignof(std::uint32_t));

/// @brief Query-to-tile grouping supplied with the exact sketch submission. runtime
/// derives and seals the final query records from this layout plus the device
/// count/scan output; coordinate callers cannot repartition the resident image.
struct gpu_sketch_query_layout_wire {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
    std::uint32_t query_len = 0;
};

/// @brief Move-only authority for one device-only query sketch.  Only runtime can
/// issue it, after the exact count/scan/emit submissions have completed.  The
/// authority is consumed by the persisted grouped-SoA coordinate operation; a
/// copied dispatch counter can therefore never stand in for the resident
/// query buffer that produced the operation.
class device_query_sketch_authorization {
public:
    device_query_sketch_authorization() = default;
    device_query_sketch_authorization(const device_query_sketch_authorization&) = delete;
    device_query_sketch_authorization& operator=(
        const device_query_sketch_authorization&) = delete;
    device_query_sketch_authorization(device_query_sketch_authorization&& other) noexcept {
        move_from(other);
    }
    device_query_sketch_authorization& operator=(
        device_query_sketch_authorization&& other) noexcept {
        if (this != &other) {
            clear();
            move_from(other);
        }
        return *this;
    }

    /// @brief Structural, device-output coverage. This deliberately does not claim
    /// the immutable pipeline provenance required by persisted grouped-SoA.
    /// @return The computed result.
    [[nodiscard]] bool does_cover_device_output() const noexcept {
        const bool shape_valid = identity_ != 0u
            && runtime_ != nullptr
            && buffer_ != VK_NULL_HANDLE
            && memory_identity_ != 0u
            && buffer_bytes_ != 0u
            && allocation_bytes_ >= buffer_bytes_
            && materialization_
                == vulkan_sketch_host_materialization::device_only
            && dispatch_stats_.is_internally_consistent()
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
    /// @brief Strong one-shot authority accepted by the persisted grouped-SoA consumer.
    /// @return The computed result.
    [[nodiscard]] bool is_valid() const noexcept {
        return does_cover_device_output()
            && pipeline_token_.is_valid_for_query_sketch();
    }
    [[nodiscard]] std::uint64_t identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        constexpr std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        if (query_records_.capacity()
                > maximum / sizeof(gpu_seed_query_record_wire)
            || query_tile_counts_.capacity()
                > maximum / sizeof(std::uint32_t)) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        const std::uint64_t record_bytes =
            static_cast<std::uint64_t>(query_records_.capacity())
            * sizeof(gpu_seed_query_record_wire);
        const std::uint64_t tile_count_bytes =
            static_cast<std::uint64_t>(query_tile_counts_.capacity())
            * sizeof(std::uint32_t);
        return tile_count_bytes > maximum - record_bytes
            ? maximum
            : record_bytes + tile_count_bytes;
    }

private:
    friend class runtime;

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
            vulkan_sketch_host_materialization::host_and_device;
        dispatch_stats_ = runtime_sketch_dispatch_stats{};
        pipeline_token_ = runtime_pipeline_operation_token{};
        device_uuid_.fill(0u);
        query_records_.clear();
        query_tile_counts_.clear();
    }
    void move_from(device_query_sketch_authorization& other) noexcept {
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
        dispatch_stats_ = other.dispatch_stats_;
        pipeline_token_ = std::move(other.pipeline_token_);
        device_uuid_ = other.device_uuid_;
        query_records_ = std::move(other.query_records_);
        query_tile_counts_ = std::move(other.query_tile_counts_);
        other.clear();
    }

    std::uint64_t identity_ = 0u;
    runtime* runtime_ = nullptr;
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
    vulkan_sketch_host_materialization materialization_ =
        vulkan_sketch_host_materialization::host_and_device;
    runtime_sketch_dispatch_stats dispatch_stats_{};
    runtime_pipeline_operation_token pipeline_token_{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid_{};
    std::vector<gpu_seed_query_record_wire> query_records_{};
    std::vector<std::uint32_t> query_tile_counts_{};
};

static_assert(!std::is_copy_constructible_v<device_query_sketch_authorization>);
static_assert(std::is_nothrow_move_constructible_v<
    device_query_sketch_authorization>);

/// @brief Resident and optionally host-materialized outputs from a sketch batch.
struct runtime_sketch_batch_result {
    buffer minimizer_words;
    std::vector<std::byte> host_minimizer_bytes;
    std::vector<std::uint32_t> tile_counts;
    std::vector<std::uint32_t> tile_offsets;
    runtime_sketch_dispatch_stats dispatch_stats{};
    device_query_sketch_authorization query_authority{};
    std::uint64_t count = 0;
    bool overflow = false;
};

/// @brief Physical admission plan for one batched GPU frontend sketch.  The arena
/// allocation includes any larger persistent high-water buffer already owned
/// by the runtime; the output allocation uses the runtime-private compute
/// buffer usage flags rather than asking callers to reproduce them.
struct gpu_sketch_batch_allocation_requirements {
    bool valid = false;
    std::uint64_t arena_requested_bytes = 0;
    std::uint64_t arena_allocation_bytes = 0;
    std::uint64_t pessimistic_output_requested_bytes = 0;
    std::uint64_t pessimistic_output_allocation_bytes = 0;
};

/// @brief Inputs to the seed-hit lookup roundtrip. The query minimizers
/// arrive either as a resident device buffer (`device_minimizer_words`,
/// all-Vulkan sketch+seed path) or as host bytes to upload once. `slots` and
/// `positions` are the GPU-resident index buffers owned by
/// `gpu_minimizer_index_cache`; the runtime never uploads the index itself.
struct runtime_seed_lookup_inputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const buffer* device_minimizer_words = nullptr;
    const buffer* slots = nullptr;
    const buffer* positions = nullptr;
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
// Runtime-owned GPU arena
// ===========================================================================

/// @brief A view into one arena buffer: `(buffer, offset, size)`. Descriptor writes
/// bind exactly this sub-range, so several logical resources can share one
/// physical arena buffer.
struct gpu_arena_slice {
    buffer* buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    [[nodiscard]] bool is_valid() const noexcept { return buffer != nullptr; }
};

/// @brief A snapshot of the three arena cursors. `rewind(mark)` restores all three.
struct gpu_arena_mark {
    std::uint64_t device_cursor = 0;
    std::uint64_t upload_cursor = 0;
    std::uint64_t download_cursor = 0;
};

/// @brief Per-runtime arena with three geometrically-grown, persistently-mapped
/// buffers (device-local, host-visible upload, host-visible download). Slices
/// are bump-allocated with alignment; `rewind` frees everything past a mark.
/// Growth is allowed only outside an active count and execution submission pair.
class gpu_arena {
public:
    explicit gpu_arena(runtime& runtime) noexcept : runtime_(&runtime) {}

    [[nodiscard]] gpu_arena_mark mark() const noexcept {
        return gpu_arena_mark{device_cursor_, upload_cursor_, download_cursor_};
    }
    void rewind(gpu_arena_mark mark) noexcept {
        device_cursor_ = mark.device_cursor;
        upload_cursor_ = mark.upload_cursor;
        download_cursor_ = mark.download_cursor;
    }

    /// @brief Reserve worst-case capacity for one count and execution submission pair
    /// before count submit,
    /// so no buffer is reallocated between the two submits. Returns false if any
    /// grow failed (caller declines GPU frontend).
    /// @return The computed result.
    [[nodiscard]] bool ensure_capacity(
        std::uint64_t device_bytes, std::uint64_t upload_bytes,
        std::uint64_t download_bytes);

    [[nodiscard]] gpu_arena_slice allocate_device(
        std::uint64_t bytes, std::uint64_t alignment);
    [[nodiscard]] gpu_arena_slice allocate_upload(std::uint64_t bytes, std::uint64_t alignment);
    [[nodiscard]] gpu_arena_slice allocate_download(std::uint64_t bytes, std::uint64_t alignment);

    void bind_storage_slice(
        VkWriteDescriptorSet& write, VkDescriptorBufferInfo& info,
        std::uint32_t binding, VkDescriptorSet dst_set,
        const gpu_arena_slice& slice) const noexcept;

    [[nodiscard]] buffer& device_buffer() noexcept { return device_; }
    [[nodiscard]] buffer& upload_buffer() noexcept { return upload_; }
    [[nodiscard]] buffer& download_buffer() noexcept { return download_; }

    /// @brief Destroy every retained backing buffer after an operation has rewound all
    /// of its slices. Seed lookup is a useful full-lifetime boundary: its output
    /// is copied to owned host storage, while adopted target-index buffers are
    /// standalone allocations and therefore cannot alias this arena. Releasing
    /// the idle arena prevents large transient high-water from leaking across
    /// otherwise independent benchmark/production requests.
    /// @return The computed result.
    [[nodiscard]] bool release_idle_capacity() noexcept;

private:
    [[nodiscard]] static std::uint64_t align_up(
        std::uint64_t value,
        std::uint64_t alignment) noexcept {
        if (alignment <= 1u) return value;
        return bioseq::common::checked_align_up(value, alignment).value_or(
            (std::numeric_limits<std::uint64_t>::max)());
    }

    runtime* runtime_ = nullptr;
    buffer device_;
    buffer upload_;
    buffer download_;
    std::uint64_t device_cursor_ = 0;
    std::uint64_t upload_cursor_ = 0;
    std::uint64_t download_cursor_ = 0;
    std::uint64_t device_capacity_ = 0;
    std::uint64_t upload_capacity_ = 0;
    std::uint64_t download_capacity_ = 0;
};

/// @brief RAII arena reservation: holds the admission permit, marks the arena on
/// construction, and rewinds on destruction. Spans count submit + execution submit.
class gpu_arena_reservation {
public:
    gpu_arena_reservation(gpu_arena& arena, bioseq::parallelism::memory_permit permit) noexcept
        : arena_(&arena), permit_(std::move(permit)), mark_(arena.mark()) {}
    gpu_arena_reservation(const gpu_arena_reservation&) = delete;
    gpu_arena_reservation& operator=(const gpu_arena_reservation&) = delete;
    gpu_arena_reservation(gpu_arena_reservation&& other) noexcept
        : arena_(std::exchange(other.arena_, nullptr)),
          permit_(std::move(other.permit_)), mark_(other.mark_) {}
    gpu_arena_reservation& operator=(gpu_arena_reservation&& other) noexcept {
        if (this != &other) {
            release();
            arena_ = std::exchange(other.arena_, nullptr);
            permit_ = std::move(other.permit_);
            mark_ = other.mark_;
        }
        return *this;
    }
    ~gpu_arena_reservation() { release(); }
    void release() noexcept {
        if (arena_ != nullptr) {
            arena_->rewind(mark_);
            arena_ = nullptr;
        }
    }
    [[nodiscard]] gpu_arena& arena() noexcept { return *arena_; }

private:
    gpu_arena* arena_ = nullptr;
    bioseq::parallelism::memory_permit permit_;
    gpu_arena_mark mark_{};
};

/// @brief Request-boundary cleanup for operations whose result never aliases the
/// shared GPU frontend arena.  Rewinding alone leaves the arena's allocator-rounded
/// high-water live; this guard also destroys all idle backing buffers.  It is
/// intentionally independent of an admission permit so it can be installed
/// before arena growth and covers every return and exception path.
class gpu_arena_trim_guard {
public:
    explicit gpu_arena_trim_guard(gpu_arena& arena) noexcept
        : arena_(&arena), mark_(arena.mark()) {}
    gpu_arena_trim_guard(const gpu_arena_trim_guard&) = delete;
    gpu_arena_trim_guard& operator=(const gpu_arena_trim_guard&) = delete;
    gpu_arena_trim_guard(gpu_arena_trim_guard&&) = delete;
    gpu_arena_trim_guard& operator=(gpu_arena_trim_guard&&) = delete;
    ~gpu_arena_trim_guard() { (void)release_now(); }

    [[nodiscard]] bool release_now() noexcept {
        if (arena_ == nullptr) {
            return true;
        }
        gpu_arena* arena = std::exchange(arena_, nullptr);
        arena->rewind(mark_);
        return arena->release_idle_capacity();
    }

private:
    gpu_arena* arena_ = nullptr;
    gpu_arena_mark mark_{};
};

// ===========================================================================
// Two-submit resident frontend I/O
// ===========================================================================

/// @brief Storage-free view of a GPU target index (slots/positions buffer slices plus
/// the slot mask / counts). Lets the runtime accept an index without importing
/// `bioseq::indexing` (which would create a module cycle).
struct gpu_target_index_view {
    gpu_arena_slice slots;
    gpu_arena_slice positions;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t valid = 0;
};

/// @brief Plan describing one device radix sort over parallel (key,value) uint64 arrays.
struct gpu_sort_plan {
    std::uint64_t element_count = 0;
    std::uint32_t key_bytes = 8;
    std::uint32_t value_bytes = 8;
    std::uint32_t radix_bits = 8;
    std::uint32_t pass_count = 8;
};

/// @brief Coordinate-only chain result plus the selected predecessor trace. The trace
/// entries are anchor indices in reverse chain order, matching the
/// `coordinate_reduce` shader scratch buffer.
struct gpu_coordinate_chain_trace_result {
    gpu_coordinate_result_wire result{};
    std::vector<std::uint32_t> trace{};
};

/// @brief GPU sequence-sort wire structs. Byte layout is pinned to the GLSL
/// `P` push block and `SeqDesc`/`SeqSortRange` declarations in shader_compiler.
struct sequence_sort_push_constants {   // 40 bytes; == GLSL pc (seq_sort kernels)
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
static_assert(sizeof(sequence_sort_push_constants) == 40);

/// @brief Packed sequence descriptor consumed by sequence-sort shaders.
struct sequence_descriptor_wire {   // 16 bytes; == GLSL SeqDesc. word_offset in uint32 words.
    std::uint32_t word_offset = 0;
    std::uint32_t unit_count = 0;
    std::uint32_t bp_size = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(sequence_descriptor_wire) == 16);

/// @brief One borrowed byte range participating in a sequence-sort upload.
struct sequence_sort_upload_chunk {
    const void* bytes = nullptr;
    std::uint64_t byte_size = 0;
    std::uint64_t padded_byte_size = 0;
};

/// @brief Resolved by the façade from compact_sequence_range<S>::traits.
struct sequence_sort_kernel_parameters {
    std::uint32_t alphabet_kind = 0;   // alphabet_kernel_kind: dna4_nt4 -> _dna4 kinds; else _generic
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t char_mask = 0;
    std::uint64_t used_storage_mask = 0;
};

/// @brief Result of a GPU-resident target minimizer index build. Owns the
/// open-addressed slot table and the hash-run-ordered positions buffer. The
/// indexing layer adopts these into a `gpu_resident_minimizer_index` outside the
/// runtime module (which must not import `bioseq::indexing`).
enum class gpu_index_build_failure : std::uint8_t {
    none,
    invalid_shape,
    minimizer_capacity_exceeds_u32,
    memory_plan_invalid,
    device_arena_allocation_failed,
    output_capacity_overflow,
    device_buffer_allocation_failed,
    count_peak_memory_admission_denied,
    execution_peak_memory_admission_denied,
    peak_allocation_exceeds_admission,
    index_invariant_failed,
};

/// @brief Canonical query-gate identity carried with every GPU-built index.  The
/// digest accelerates rejection, but immutable exact words remain authoritative.
inline constexpr std::uint32_t k_gpu_query_gate_bits = 26u;
inline constexpr std::uint32_t k_gpu_query_gate_word_count =
    (std::uint32_t{1} << k_gpu_query_gate_bits) / 32u;
inline constexpr std::uint32_t k_gpu_query_gate_mask =
    (std::uint32_t{1} << k_gpu_query_gate_bits) - 1u;

/// @brief Immutable canonical query-gate identity for a GPU-built index.
struct gpu_index_gate_identity {
    bool enabled = false;
    std::uint32_t word_count = 0u;
    std::uint32_t mask = 0u;
    std::uint64_t digest_lo = 0u;
    std::uint64_t digest_hi = 0u;
    std::shared_ptr<const std::vector<std::uint32_t>> canonical_words{};

    [[nodiscard]] bool is_well_formed() const noexcept {
        if (!enabled) {
            return word_count == 0u && mask == 0u
                && digest_lo == 0u && digest_hi == 0u
                && !canonical_words;
        }
        return word_count == k_gpu_query_gate_word_count
            && mask == k_gpu_query_gate_mask && canonical_words
            && canonical_words->size() == word_count;
    }

    [[nodiscard]] bool is_exactly_equal_to(
        const gpu_index_gate_identity& other) const
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

/// @brief Dispatch, admission, and lifetime statistics for one GPU index build.
struct gpu_index_build_stats {
    std::uint64_t count_submit_admissions = 0;
    std::uint64_t execution_peak_admissions = 0;
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
    // Deterministic host-scratch lifetime evidence for target-index builds.
    // A prior idle arena high-water is destroyed before count admission; the
    // packed sequence is copied directly from the borrowed input and only its
    // device padding is zeroed; tile records are filled directly in mapped
    // arena storage, so exact execution grow_to sees no host tile owner.
    std::uint64_t idle_arena_trim_attempts_before_count = 0;
    std::uint64_t idle_arena_trim_successes_before_count = 0;
    std::uint64_t idle_arena_released_bytes_before_count = 0;
    std::uint64_t pretrim_peak_observed_bytes = 0;
    std::uint64_t direct_sequence_arena_copies = 0;
    std::uint64_t sequence_padding_zeroed_bytes = 0;
    std::uint64_t host_sequence_capacity_releases_before_execution = 0;
    std::uint64_t host_sequence_capacity_released_bytes = 0;
    std::uint64_t direct_tile_arena_fills = 0;
    std::uint64_t host_tile_staging_elided_bytes = 0;
    std::uint64_t count_submit_host_transient_bytes = 0;
    std::uint64_t execution_submit_host_transient_bytes = 0;
    std::uint64_t host_transient_released_before_execution_bytes = 0;
    // Set inside runtime under its mutex only after the runtime scope has
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

/// @brief Move-owned buffers, permits, and identity for a GPU-built target index.
struct gpu_built_index {
    // Declared first so ownership is released only after both buffers.
    bioseq::parallelism::memory_permit permit;
    buffer slots;
    buffer positions;
    gpu_index_gate_identity gate_identity{};
    bioseq::parallelism::memory_materialization_result materialization{};
    gpu_index_build_stats ownership{};
    std::uint64_t peak_admission_bytes = 0;
    std::uint64_t observed_device_peak_bytes = 0;
    bioseq::parallelism::memory_admission_failure execution_peak_admission_failure{};
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t occupied_slots = 0;
    std::uint32_t position_count = 0;
    gpu_index_build_failure failure_reason =
        gpu_index_build_failure::none;
    bool valid = false;

    gpu_built_index() = default;
    gpu_built_index(const gpu_built_index&) = delete;
    gpu_built_index& operator=(const gpu_built_index&) = delete;
    gpu_built_index(gpu_built_index&& other) noexcept {
        move_from(other);
    }
    gpu_built_index& operator=(gpu_built_index&& other) noexcept {
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
    void move_from(gpu_built_index& other) noexcept {
        permit = std::move(other.permit);
        slots = std::move(other.slots);
        positions = std::move(other.positions);
        gate_identity = std::move(other.gate_identity);
        materialization = other.materialization;
        ownership = other.ownership;
        peak_admission_bytes = other.peak_admission_bytes;
        observed_device_peak_bytes = other.observed_device_peak_bytes;
        execution_peak_admission_failure = other.execution_peak_admission_failure;
        target_len = other.target_len;
        slot_mask = other.slot_mask;
        occupied_slots = other.occupied_slots;
        position_count = other.position_count;
        failure_reason = other.failure_reason;
        valid = other.valid;
        other.valid = false;
    }
};
static_assert(std::is_standard_layout_v<gpu_built_index>);
static_assert(std::is_nothrow_move_constructible_v<gpu_built_index>);
static_assert(std::is_nothrow_move_assignable_v<gpu_built_index>);
static_assert(offsetof(gpu_built_index, permit) < offsetof(gpu_built_index, slots));
static_assert(offsetof(gpu_built_index, permit) < offsetof(gpu_built_index, positions));

/// @brief Host-visible result of the GPU sketch and stable two-key sort roundtrip.
/// Owns the device buffer holding the final sorted run plus the downloaded
/// host bytes (host/Postgres-format persistence happens on the CPU side).
struct gpu_sorted_pair_result {
    buffer keys;
    buffer values;
    std::vector<std::byte> host_key_bytes;
    std::vector<std::byte> host_value_bytes;
    std::uint64_t target_len = 0;
    std::uint32_t pair_count = 0;
    bool valid = false;
};

/// @brief Device-side ping-pong views for the stable two-key pair
/// sort. Hoisted above `class runtime` because the private method
/// `record_stable_pair_sort_by_value_then_key_locked` takes it by reference.
struct sorted_pair_device_views {
    gpu_arena_slice keys;
    gpu_arena_slice values;
    gpu_arena_slice keys_scratch;
    gpu_arena_slice values_scratch;
    bool keys_in_scratch = false;
};

[[nodiscard]] inline const gpu_arena_slice& choose_sorted_keys(
    const sorted_pair_device_views& views) noexcept
{
    return views.keys_in_scratch ? views.keys_scratch : views.keys;
}

[[nodiscard]] inline const gpu_arena_slice& choose_sorted_values(
    const sorted_pair_device_views& views) noexcept
{
    return views.keys_in_scratch ? views.values_scratch : views.values;
}

/// @brief Inputs to the GPU seed count, emit, and sort roundtrip. The query
/// minimizer words arrive resident (`device_minimizer_words`) or as host bytes;
/// the index slots/positions are the GPU-resident buffers.
struct gpu_seed_inputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const buffer* device_minimizer_words = nullptr;
    const buffer* slots = nullptr;
    const buffer* positions = nullptr;
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


/// @brief Common query-side inputs for a batched resident seed→chain→coordinate run.
/// The query minimizer words are shared by every job in the batch. Source-direct jobs
/// may supply distinct slot-table targets; an authenticated grouped-SoA direct
/// operation is intentionally restricted to one resident generation/view.
struct gpu_seed_query_inputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const buffer* device_minimizer_words = nullptr;
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t window_size = 0;
    std::span<const gpu_seed_query_record_wire> host_query_records{};
    const buffer* device_query_records = nullptr;
};

/// @brief Neutral resident view of one authenticated persisted grouped-SoA generation.
/// The owner keeps all three buffers alive for the complete batch.  Byte sizes
/// are logical admitted sizes; the backing raw control/position buffers may be
/// padded to whole uint32 words for GLSL access, but their representation is
/// never decoded or rebuilt by this runtime.
struct gpu_grouped_soa_resident_view {
    const buffer* descriptors = nullptr;
    const buffer* controls = nullptr;
    const buffer* positions = nullptr;
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_revision = 0;
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
};

/// @brief Mutually exclusive resident target-index representations accepted by seeding.
enum class gpu_seed_index_representation : std::uint8_t {
    invalid = 0,
    source_direct_sorted_dense = 1,
    grouped_soa = 2,
};

/// @brief One resident target-index job in a batched seed→coordinate run. Source-direct
/// slot-table fields and `grouped_soa` are an XOR: selecting both (or neither) is
/// invalid rather than permission to fall back to another implementation.
struct gpu_seed_coordinate_job {
    const buffer* slots = nullptr;
    const buffer* positions = nullptr;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t bucket_count = 0;
    std::uint64_t target_position_offset = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    const gpu_grouped_soa_resident_view* grouped_soa = nullptr;
};

[[nodiscard]] inline constexpr gpu_seed_index_representation
classify_gpu_seed_index_representation(
    const gpu_seed_coordinate_job& job) noexcept
{
    const bool grouped_soa_only = job.grouped_soa != nullptr
        && job.slots == nullptr
        && job.positions == nullptr
        && job.slot_mask == 0u
        && job.position_count == 0u
        && job.bucket_count == 0u
        && job.target_position_offset == 0u;
    if (grouped_soa_only) return gpu_seed_index_representation::grouped_soa;

    const bool source_direct_only = job.grouped_soa == nullptr
        && job.slots != nullptr
        && job.positions != nullptr;
    return source_direct_only
        ? gpu_seed_index_representation::source_direct_sorted_dense
        : gpu_seed_index_representation::invalid;
}

/// @brief Explicit algorithm identity for the seed-to-coordinate backend.  A caller
/// selecting Vulkan tiled-DP must pass `vulkan_tiled_dp`; the approximate
/// coordinate-vote prototype is a distinct algorithm and may never be entered
/// implicitly as an optimization of tiled-DP.
enum class gpu_seed_coordinate_algorithm : std::uint8_t {
    vulkan_tiled_dp = 0,
    coordinate_vote = 1,
};

/// @brief Exact algorithm inputs bound into a persisted grouped-SoA coordinate receipt.
/// Floating-point penalties are carried by bit pattern so NaNs, signed zero,
/// and every other dispatch-affecting value compare without normalization.
struct persisted_grouped_soa_coordinate_config_identity {
    std::uint32_t kmer_size = 0u;
    std::int32_t max_occ = -1;
    gpu_seed_coordinate_algorithm coordinate_algorithm =
        gpu_seed_coordinate_algorithm::vulkan_tiled_dp;
    chaining_pipeline_key chain_key{};
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

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return kmer_size != 0u && kmer_size <= 255u
            && max_occ >= 0
            && coordinate_algorithm
                == gpu_seed_coordinate_algorithm::vulkan_tiled_dp
            && chain_key.tile_size != 0u
            && chain_key.local_size != 0u
            && probe_limit != 0u;
    }
    [[nodiscard]] bool operator==(
        const persisted_grouped_soa_coordinate_config_identity&) const = default;
};

[[nodiscard]] inline persisted_grouped_soa_coordinate_config_identity
make_persisted_grouped_soa_coordinate_config_identity(
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    gpu_seed_coordinate_algorithm coordinate_algorithm,
    const chaining_pipeline_key& chain_key,
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
    return persisted_grouped_soa_coordinate_config_identity{
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

static_assert(!persisted_grouped_soa_coordinate_config_identity{}.is_valid());

/// @brief Dispatch counts emitted by one seed-to-coordinate batch.
struct gpu_seed_coordinate_batch_stats {
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

[[nodiscard]] inline constexpr bool are_coordinate_result_wires_equal(
    const gpu_coordinate_result_wire& lhs,
    const gpu_coordinate_result_wire& rhs) noexcept {
    return lhs.query_start == rhs.query_start
        && lhs.query_end == rhs.query_end
        && lhs.target_start == rhs.target_start
        && lhs.target_end == rhs.target_end
        && lhs.match_length == rhs.match_length
        && lhs.block_length == rhs.block_length
        && lhs.chain_score == rhs.chain_score
        && lhs.seed_count == rhs.seed_count
        && lhs.is_reverse == rhs.is_reverse
        && lhs.is_valid == rhs.is_valid;
}

[[nodiscard]] inline constexpr bool is_coordinate_result_wire_well_formed(
    const gpu_coordinate_result_wire& result,
    const std::uint64_t query_len,
    const std::uint64_t target_len) noexcept {
    if (result.is_valid == 0u) {
        return are_coordinate_result_wires_equal(
            result, gpu_coordinate_result_wire{});
    }
    if (result.is_valid != 1u || result.is_reverse > 1u
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

/// @brief Exact caller-side identities checked while consuming one sealed operation
/// record.  These are expectations, not receipt: changing any value makes the
/// one-shot consume fail.
struct persisted_grouped_soa_coordinate_job_expectation {
    std::size_t problem_index = 0;
    std::uint64_t query_authority_identity = 0;
    runtime_buffer_identity resident_descriptors{};
    runtime_buffer_identity resident_controls{};
    runtime_buffer_identity resident_positions{};
    runtime_buffer_identity query_minimizer_buffer{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_revision = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint64_t target_len = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    gpu_seed_query_record_wire query_record{};
    gpu_coordinate_result_wire result{};
    persisted_grouped_soa_coordinate_config_identity configuration{};
};

/// @brief Operation-local audit payload.  runtime fills this from successful Vulkan
/// calls; it is public only so the contract can be golden/tamper tested.  It
/// cannot be converted into an operation receipt by callers.
struct persisted_grouped_soa_coordinate_job_audit {
    std::uint64_t operation_identity = 0;
    std::uint64_t query_authority_identity = 0;
    runtime_buffer_identity resident_descriptors{};
    runtime_buffer_identity resident_controls{};
    runtime_buffer_identity resident_positions{};
    runtime_buffer_identity query_minimizer_buffer{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_revision = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint64_t target_len = 0;
    std::size_t problem_index = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    gpu_seed_query_record_wire query_record{};
    gpu_coordinate_result_wire result{};
    persisted_grouped_soa_coordinate_config_identity configuration{};
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

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        const bool identity_and_query = operation_identity != 0u
            && query_authority_identity != 0u
            && resident_descriptors.is_valid()
            && resident_controls.is_valid()
            && resident_positions.is_valid()
            && query_minimizer_buffer.is_valid()
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
                    * sizeof(gpu_grouped_soa_resident_partition_descriptor)
            && abi_revision == k_gpu_grouped_soa_abi_revision
            && generation_id != 0u
            && source_identity != 0u
            && cold_load_identity != 0u
            && target_len != 0u
            && query_record.flags == 0u
            && configuration.is_valid()
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
                && are_coordinate_result_wires_equal(
                    result, gpu_coordinate_result_wire{});
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
                && are_coordinate_result_wires_equal(
                    result, gpu_coordinate_result_wire{});
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
            && is_coordinate_result_wire_well_formed(
                result, query_record.query_len, target_len);
    }
};

/// @brief Unforgeable consumed view of one accepted audit record.
class persisted_grouped_soa_coordinate_job_receipt {
public:
    persisted_grouped_soa_coordinate_job_receipt() = default;
    persisted_grouped_soa_coordinate_job_receipt(
        const persisted_grouped_soa_coordinate_job_receipt&) = delete;
    persisted_grouped_soa_coordinate_job_receipt& operator=(
        const persisted_grouped_soa_coordinate_job_receipt&) = delete;
    persisted_grouped_soa_coordinate_job_receipt(
        persisted_grouped_soa_coordinate_job_receipt&& other) noexcept {
        move_from(other);
    }
    persisted_grouped_soa_coordinate_job_receipt& operator=(
        persisted_grouped_soa_coordinate_job_receipt&& other) noexcept {
        if (this != &other) {
            audit_ = {};
            pipeline_operation_identity_ = 0u;
            issued_ = false;
            move_from(other);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && pipeline_operation_identity_ != 0u
            && audit_.is_internally_consistent();
    }
    [[nodiscard]] const persisted_grouped_soa_coordinate_job_audit& audit()
        const noexcept {
        return audit_;
    }

private:
    friend class persisted_grouped_soa_coordinate_operation_receipt;

    void move_from(persisted_grouped_soa_coordinate_job_receipt& other) noexcept {
        audit_ = other.audit_;
        issued_ = other.issued_;
        pipeline_operation_identity_ = other.pipeline_operation_identity_;
        other.audit_ = {};
        other.issued_ = false;
        other.pipeline_operation_identity_ = 0u;
    }

    persisted_grouped_soa_coordinate_job_audit audit_{};
    std::uint64_t pipeline_operation_identity_ = 0u;
    bool issued_ = false;
};

/// @brief Aggregate acceptance accounting for a grouped-SoA coordinate operation.
struct persisted_grouped_soa_coordinate_operation_acceptance_audit {
    std::uint64_t operation_identity = 0u;
    std::uint64_t expected_job_count = 0u;
    std::uint64_t consumed_count = 0u;
    std::uint64_t accepted_count = 0u;
    std::uint64_t burned_count = 0u;
    std::uint64_t query_pipeline_receipt_count = 0u;
    std::uint64_t shape_pipeline_receipt_count = 0u;
    std::uint64_t committed_submission_count = 0u;
    std::uint64_t seed_receipt_identity = 0u;

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
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
        const persisted_grouped_soa_coordinate_operation_acceptance_audit&)
        const noexcept = default;
};

/// @brief Allocation-free pre-finalizer view of the exact operation authority fields
/// later copied into the SQL evidence ledger.
struct persisted_grouped_soa_coordinate_operation_receipt_preview {
    persisted_grouped_soa_coordinate_operation_acceptance_audit acceptance{};
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
    runtime_metal_archive_process_qualification cache_qualification{};
    runtime_gpu_cache_metrics_audit maximum_cache_metrics{};

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return acceptance.is_internally_consistent()
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
            && cache_qualification.is_internally_consistent()
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
            && maximum_cache_metrics.is_internally_consistent()
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
        const persisted_grouped_soa_coordinate_operation_receipt_preview&)
        const noexcept = default;
};

/// @brief Exact full-stage job payload plus its structured Shape submission receipts.
struct persisted_grouped_soa_coordinate_job_receipt_preview {
    persisted_grouped_soa_coordinate_job_audit audit{};
    std::uint64_t shape_pipeline_operation_identity = 0u;
    std::array<std::uint64_t, 4> shape_submission_ordinals{};

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        if (!audit.is_internally_consistent() || audit.anchor_count == 0u
            || audit.range_count == 0u || audit.result.is_valid != 1u
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

/// @brief Move-only accepted operation and its immutable environment identity.
class persisted_grouped_soa_coordinate_accepted_operation {
public:
    persisted_grouped_soa_coordinate_accepted_operation() = default;
    persisted_grouped_soa_coordinate_accepted_operation(
        const persisted_grouped_soa_coordinate_accepted_operation&) = delete;
    persisted_grouped_soa_coordinate_accepted_operation& operator=(
        const persisted_grouped_soa_coordinate_accepted_operation&) = delete;
    persisted_grouped_soa_coordinate_accepted_operation(
        persisted_grouped_soa_coordinate_accepted_operation&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          environment_(std::exchange(other.environment_, {})),
          cache_metrics_(std::move(other.cache_metrics_)),
          issued_(std::exchange(other.issued_, false)) {}
    persisted_grouped_soa_coordinate_accepted_operation& operator=(
        persisted_grouped_soa_coordinate_accepted_operation&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            environment_ = std::exchange(other.environment_, {});
            cache_metrics_ = std::move(other.cache_metrics_);
            issued_ = std::exchange(other.issued_, false);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && audit_.is_internally_consistent() && environment_.is_internally_consistent()
            && cache_metrics_.is_valid();
    }
    [[nodiscard]] const persisted_grouped_soa_coordinate_operation_acceptance_audit&
    audit() const noexcept { return audit_; }
    [[nodiscard]] const runtime_gpu_execution_environment_snapshot&
    environment_binding() const noexcept { return environment_; }
    [[nodiscard]] const runtime_gpu_cache_metrics_audit&
    cache_metrics_audit() const noexcept { return cache_metrics_.audit(); }
    [[nodiscard]] runtime_gpu_cache_metrics_delta
    take_cache_metrics_delta() noexcept {
        return std::move(cache_metrics_);
    }

private:
    persisted_grouped_soa_coordinate_operation_acceptance_audit audit_{};
    runtime_gpu_execution_environment_snapshot environment_{};
    runtime_gpu_cache_metrics_delta cache_metrics_{};
    bool issued_ = false;
    friend class runtime;
};

static_assert(!std::is_copy_constructible_v<
    persisted_grouped_soa_coordinate_accepted_operation>);
static_assert(std::is_nothrow_move_constructible_v<
    persisted_grouped_soa_coordinate_accepted_operation>);

/// @brief runtime-sealed, move-only receipt for one grouped-SoA coordinate batch.  Each
/// logical job can be consumed exactly once and only with matching resident,
/// query-record and result identities.
class persisted_grouped_soa_coordinate_operation_receipt {
public:
    persisted_grouped_soa_coordinate_operation_receipt() = default;
    persisted_grouped_soa_coordinate_operation_receipt(
        const persisted_grouped_soa_coordinate_operation_receipt&) = delete;
    persisted_grouped_soa_coordinate_operation_receipt& operator=(
        const persisted_grouped_soa_coordinate_operation_receipt&) = delete;
    persisted_grouped_soa_coordinate_operation_receipt(
        persisted_grouped_soa_coordinate_operation_receipt&& other) noexcept {
        move_from(other);
    }
    persisted_grouped_soa_coordinate_operation_receipt& operator=(
        persisted_grouped_soa_coordinate_operation_receipt&& other) noexcept {
        if (this != &other) {
            release_records();
            query_pipeline_token_ = {};
            grouped_soa_pipeline_token_ = {};
            move_from(other);
        }
        return *this;
    }

    [[nodiscard]] bool is_sealed() const noexcept {
        return sealed_ && operation_identity_ != 0u;
    }
    [[nodiscard]] std::size_t job_count() const noexcept {
        return is_sealed() ? records_.size() : 0u;
    }
    [[nodiscard]] std::size_t consumed_count() const noexcept {
        return is_sealed() ? accepted_count_ + burned_count_ : 0u;
    }
    [[nodiscard]] std::size_t accepted_count() const noexcept {
        return is_sealed() ? accepted_count_ : 0u;
    }
    [[nodiscard]] std::size_t burned_count() const noexcept {
        return is_sealed() ? burned_count_ : 0u;
    }
    [[nodiscard]] bool is_fully_consumed() const noexcept {
        return is_sealed()
            && !records_.empty()
            && std::all_of(
                records_.begin(),
                records_.end(), [](const auto& record) {
                    return record.state != record_state::fresh;
                });
    }
    [[nodiscard]] bool are_all_jobs_accepted() const noexcept {
        return is_sealed() && is_fully_consumed()
            && accepted_count_ == records_.size()
            && burned_count_ == 0u && has_valid_causal_tokens();
    }
    [[nodiscard]] runtime_gpu_execution_environment_snapshot
    environment_preview() const noexcept {
        return preview_environment();
    }
    [[nodiscard]] runtime_gpu_cache_metrics_audit
    cache_metrics_preview() const noexcept {
        return maximum_cache_metrics();
    }
    [[nodiscard]] persisted_grouped_soa_coordinate_operation_receipt_preview
    operation_receipt_preview() const noexcept {
        persisted_grouped_soa_coordinate_operation_receipt_preview result{};
        if (!are_all_jobs_accepted()
            || !query_pipeline_token_.is_valid_for_query_sketch()
            || !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return result;
        }
        const runtime_pipeline_operation_audit& query =
            query_pipeline_token_.audit();
        const runtime_pipeline_operation_audit& shape =
            grouped_soa_pipeline_token_.audit();
        if (query.jobs.size() != 1u
            || query.jobs.front().committed_submissions != 2u
            || shape.committed_submissions
                > (std::numeric_limits<std::uint64_t>::max)()
                    - query.committed_submissions) {
            return result;
        }
        result.acceptance =
            persisted_grouped_soa_coordinate_operation_acceptance_audit{
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
        return result.is_internally_consistent()
            ? result
            : persisted_grouped_soa_coordinate_operation_receipt_preview{};
    }
    [[nodiscard]] std::optional<
        persisted_grouped_soa_coordinate_job_receipt_preview>
    job_evidence_preview(const std::size_t problem_index) const noexcept {
        if (!are_all_jobs_accepted() || problem_index >= records_.size()
            || !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return std::nullopt;
        }
        const runtime_pipeline_operation_audit& shape =
            grouped_soa_pipeline_token_.audit();
        if (problem_index >= shape.jobs.size()) return std::nullopt;
        const runtime_pipeline_job_use_audit& use = shape.jobs[problem_index];
        if (use.committed_submissions != 4u
            || use.submission_operation_identity
                != shape.operation_identity) {
            return std::nullopt;
        }
        persisted_grouped_soa_coordinate_job_receipt_preview result{
            .audit = records_[problem_index].audit,
            .shape_pipeline_operation_identity =
                use.submission_operation_identity,
            .shape_submission_ordinals = use.submission_ordinals,
        };
        return result.is_internally_consistent()
            ? std::optional<
                persisted_grouped_soa_coordinate_job_receipt_preview>{result}
            : std::nullopt;
    }

    /// @brief Contract-only test hook.  It returns no receipt or evidence and therefore
    /// grants no runtime authority; it only lets the portable suite exercise
    /// private sealing, move invalidation, tamper burning, and one-shot consume.
    /// @return The computed result.
    [[nodiscard]] static bool self_test_one_shot_contract(
        const std::array<persisted_grouped_soa_coordinate_job_audit, 2>& audits,
        const std::array<persisted_grouped_soa_coordinate_job_expectation, 2>&
            expectations) {
        if (audits[0].operation_identity == 0u
            || audits[0].operation_identity != audits[1].operation_identity
            || audits[0].problem_index != 0u
            || audits[1].problem_index != 1u
            || !audits[0].is_internally_consistent()
            || !audits[1].is_internally_consistent()) {
            return false;
        }
        persisted_grouped_soa_coordinate_operation_receipt issued{};
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

        persisted_grouped_soa_coordinate_operation_receipt receipt{std::move(issued)};
        if (issued.is_sealed() || issued.job_count() != 0u
            || issued.records_capacity_permit_
            || !receipt.records_capacity_permit_
            || !receipt.is_sealed() || receipt.job_count() != 2u) {
            return false;
        }
        auto first = receipt.consume(expectations[0]);
        if (!first.has_value() || !first->is_valid()) return false;
        if (receipt.is_fully_consumed()) return false;
        persisted_grouped_soa_coordinate_job_receipt moved_first{
            std::move(*first)};
        if (first->is_valid() || !moved_first.is_valid()) return false;
        if (receipt.consume(expectations[0]).has_value()) return false;

        auto tampered_second = expectations[1];
        tampered_second.source_identity ^= 1u;
        if (receipt.consume(tampered_second).has_value()) return false;
        if (receipt.consume(expectations[1]).has_value()) return false;
        if (!receipt.is_fully_consumed() || receipt.accepted_count() != 1u
            || receipt.burned_count() != 1u
            || receipt.are_all_jobs_accepted()) return false;

        persisted_grouped_soa_coordinate_operation_receipt all_valid{};
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
        return valid_first.has_value() && valid_first->is_valid()
            && valid_second.has_value() && valid_second->is_valid()
            && !all_valid.consume(expectations[0]).has_value()
            && !all_valid.consume(expectations[1]).has_value()
            && all_valid.is_fully_consumed()
            && all_valid.accepted_count() == 2u
            && all_valid.burned_count() == 0u
            && all_valid.records_capacity_permit_
            && all_valid.are_all_jobs_accepted();
    }

    [[nodiscard]] std::optional<persisted_grouped_soa_coordinate_job_receipt>
    consume(const persisted_grouped_soa_coordinate_job_expectation& expected) noexcept {
        if (!is_sealed() || expected.problem_index >= records_.size()) {
            return std::nullopt;
        }
        record& record = records_[expected.problem_index];
        if (record.state != record_state::fresh) return std::nullopt;
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
            && audit.abi_revision == expected.abi_revision
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
            && are_coordinate_result_wires_equal(audit.result, expected.result)
            && audit.configuration == expected.configuration;
        if (!identities_match || !audit.is_internally_consistent()
            || !has_valid_causal_tokens()) {
            record.state = record_state::burned;
            ++burned_count_;
            return std::nullopt;
        }
        record.state = record_state::accepted;
        ++accepted_count_;
        persisted_grouped_soa_coordinate_job_receipt evidence{};
        evidence.audit_ = audit;
        evidence.pipeline_operation_identity_ = operation_identity_;
        evidence.issued_ = true;
        return evidence;
    }

private:
    friend class runtime;

    enum class record_state : std::uint8_t {
        fresh,
        accepted,
        burned,
    };

    struct record {
        persisted_grouped_soa_coordinate_job_audit audit{};
        record_state state = record_state::fresh;
    };

    struct pipeline_receipt_union {
        std::array<shader_variant_key,
                   k_runtime_pipeline_prepared_slots> keys{};
        std::array<const runtime_metal_archive_pipeline_receipt*,
                   k_runtime_pipeline_prepared_slots> receipts{};
        std::size_t count = 0u;
        bool valid = false;
    };

    [[nodiscard]] pipeline_receipt_union receipt_union() const noexcept {
        pipeline_receipt_union result{};
        if (!sealed_ || test_causal_accepted_
            || !query_pipeline_token_.is_valid_for_query_sketch()
            || !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return result;
        }
        const auto merge = [&result](
            const runtime_pipeline_operation_token& operation) {
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
        if (!merge(query_pipeline_token_)
            || !merge(grouped_soa_pipeline_token_)
            || result.count == 0u
            || result.count
                != grouped_soa_pipeline_token_.audit_.prepared_count + 1u) {
            return {};
        }
        result.valid = true;
        return result;
    }

    [[nodiscard]] runtime_gpu_execution_environment_snapshot
    preview_environment() const noexcept {
        const pipeline_receipt_union union_receipts = receipt_union();
        if (!union_receipts.valid) return {};
        const runtime_pipeline_operation_audit& query =
            query_pipeline_token_.audit_;
        const runtime_pipeline_operation_audit& shape =
            grouped_soa_pipeline_token_.audit_;
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
        vulkan_feature_set identity{};
        identity.api_version = query.api_version;
        identity.driver_version = query.driver_version;
        identity.vendor_id = query.vendor_id;
        identity.device_id = query.device_id;
        identity.pipeline_cache_uuid = query.pipeline_cache_uuid;
        runtime_gpu_execution_environment_snapshot result{
            .gpu_environment_present = true,
            .gpu_device_uuid = query.device_uuid,
            .gpu_driver_identity =
                canonical_vulkan_driver_identity(identity),
            .cache_policy = runtime_pipeline_cache_policy::
                immutable_preseed_all_hit,
            .seed_sha256 = query.seed_sha256,
            .seed_size = query.seed_size,
            .manifest_checksum = query.manifest_checksum,
            .pipeline_cache_all_hit = true,
        };
        return result.is_internally_consistent() ? result
                                 : runtime_gpu_execution_environment_snapshot{};
    }

    [[nodiscard]] runtime_gpu_cache_metrics_audit
    maximum_cache_metrics() const noexcept {
        const pipeline_receipt_union union_receipts = receipt_union();
        if (!union_receipts.valid) return {};
        runtime_gpu_cache_metrics_audit result{
            .token_identity = operation_identity_,
            .gpu_pipeline_cache_loads = 1u,
            .gpu_pipeline_cache_seed_bytes =
                query_pipeline_token_.audit_.seed_size,
        };
        const runtime_metal_archive_pipeline_receipt* first =
            union_receipts.receipts[0];
        if (first == nullptr || !first->is_valid()) return {};
        result.gpu_pipeline_cache_load_ns = first->audit().seed_load_ns;
        for (std::size_t i = 0u; i < union_receipts.count; ++i) {
            const runtime_metal_archive_pipeline_receipt* receipt =
                union_receipts.receipts[i];
            if (receipt == nullptr || !receipt->is_valid()
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
        return result.is_internally_consistent()
            ? result
            : runtime_gpu_cache_metrics_audit{};
    }

    void admit_records(std::size_t record_count) {
        using bioseq::common::saturating_mul;
        if (record_count == 0u || !records_.empty()
            || records_capacity_permit_
            || record_count > records_.max_size()) {
            throw vulkan_runtime_error(
                "persisted grouped-SoA operation record shape is invalid");
        }
        const std::uint64_t logical = saturating_mul(
            static_cast<std::uint64_t>(record_count), sizeof(record));
        const std::uint64_t peak = saturating_mul(
            logical,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (logical == (std::numeric_limits<std::uint64_t>::max)()
            || peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw vulkan_runtime_error(
                "persisted grouped-SoA operation record admission saturated");
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(peak, &failure);
        if (!permit) {
            throw vulkan_runtime_error(shader_cache_message(
                "persisted grouped-SoA operation record admission denied: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(
                    failure.reason),
                " requested=", failure.requested_bytes));
        }
        try {
            records_.resize(record_count);
        } catch (const std::bad_alloc&) {
            std::vector<record> empty;
            records_.swap(empty);
            throw vulkan_runtime_error(
                "persisted grouped-SoA operation record allocation failed after admission");
        }
        const std::uint64_t retained = saturating_mul(
            static_cast<std::uint64_t>(records_.capacity()),
            sizeof(record));
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || retained > permit->bytes()) {
            std::vector<record> empty;
            records_.swap(empty);
            throw vulkan_runtime_error(
                "persisted grouped-SoA operation records exceeded admitted capacity");
        }
        permit->shrink_to(retained);
        records_capacity_permit_ = std::move(*permit);
    }

    void release_records() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            records_, records_capacity_permit_);
    }

    [[nodiscard]] bool has_valid_causal_tokens() const noexcept {
        return test_causal_accepted_
            || (query_pipeline_token_.is_valid_for_query_sketch()
                && grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate());
    }

    void move_from(persisted_grouped_soa_coordinate_operation_receipt& other) noexcept {
        operation_identity_ = other.operation_identity_;
        records_ = std::move(other.records_);
        records_capacity_permit_ = std::move(
            other.records_capacity_permit_);
        query_pipeline_token_ = std::move(other.query_pipeline_token_);
        grouped_soa_pipeline_token_ = std::move(other.grouped_soa_pipeline_token_);
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
    bioseq::parallelism::memory_permit records_capacity_permit_{};
    std::vector<record> records_{};
    runtime_pipeline_operation_token query_pipeline_token_{};
    runtime_pipeline_operation_token grouped_soa_pipeline_token_{};
    std::size_t accepted_count_ = 0u;
    std::size_t burned_count_ = 0u;
    bool test_causal_accepted_ = false;
    bool sealed_ = false;
};

static_assert(!std::is_copy_constructible_v<
    persisted_grouped_soa_coordinate_operation_receipt>);
static_assert(std::is_nothrow_move_constructible_v<
    persisted_grouped_soa_coordinate_operation_receipt>);
static_assert(!persisted_grouped_soa_coordinate_job_audit{}.is_internally_consistent());

namespace runtime_detail {

/// @brief Fixed final-publication payload prepared before the immutable live gate.
/// Dropping this object without committing its claim plan has no side effects.
struct persisted_grouped_soa_coordinate_finalization_plan {
    runtime_gpu_cache_metrics_claim_plan cache_claim_plan{};
    runtime_gpu_execution_environment_snapshot environment{};
    persisted_grouped_soa_coordinate_operation_acceptance_audit acceptance{};
    bool valid = false;

    [[nodiscard]] bool is_valid() const noexcept {
        return valid && cache_claim_plan.is_valid()
            && environment.is_internally_consistent() && acceptance.is_internally_consistent();
    }
};

static_assert(!std::is_copy_constructible_v<
    persisted_grouped_soa_coordinate_finalization_plan>);
static_assert(std::is_nothrow_move_constructible_v<
    persisted_grouped_soa_coordinate_finalization_plan>);

}  // namespace runtime_detail

/// @brief Coordinate results and causal receipt produced by a batched GPU run.
struct gpu_seed_coordinate_batch_result {
    std::vector<gpu_coordinate_result_wire> results{};
    gpu_seed_coordinate_batch_stats stats{};
    persisted_grouped_soa_coordinate_operation_receipt operation_receipt{};
    /// @brief Dispatch and identity statistics for the direct grouped-SoA route.
    struct grouped_soa_direct_route_stats {
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
    } grouped_soa_direct{};
};

/// @brief The Vulkan compute runtime. One process-wide instance (`runtime::instance`)
/// owns the device and serializes GPU queue access with `mu_`.
class runtime {
public:
    /// @brief Initializes the Vulkan device and immutable runtime resources.
    runtime();
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;
    /// @brief Releases pipelines, synchronization objects, buffers, and the device.
    ~runtime();

    /// @brief Returns the process-wide Vulkan runtime instance.
    /// @return The computed result.
    [[nodiscard]] static runtime& instance();
    /// @brief Returns immutable capabilities probed during initialization.
    /// @return The computed result.
    [[nodiscard]] const vulkan_feature_set& features() const noexcept { return features_; }
    /// @brief Reports whether a usable Vulkan device was initialized.
    /// @return The computed result.
    [[nodiscard]] bool is_usable() const noexcept { return features_.available; }
    /// @brief Returns the initialization diagnostic when `is_usable()` is false.
    /// @return The computed result.
    [[nodiscard]] const std::string& initialization_error() const noexcept {
        return init_error_;
    }
    /// @brief Returns the number of committed tiled-chaining dispatches.
    /// @return The computed result.
    [[nodiscard]] std::uint64_t chaining_dispatch_count() const noexcept {
        return chaining_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns the number of committed packed-unit generator dispatches.
    /// @return The computed result.
    [[nodiscard]] std::uint64_t generate_units_dispatch_count() const noexcept {
        return generate_units_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns bytes reserved for process-lifetime pipeline-cache metadata.
    /// @return The computed result.
    [[nodiscard]] std::uint64_t pipeline_cache_metadata_reserved_bytes() {
        std::lock_guard<std::mutex> guard(mu_);
        return pipeline_cache_metadata_permit_.bytes();
    }
    /// @brief Validates and publishes a completed grouped-SoA coordinate operation.
    /// @param receipt Completed operation receipt to consume.
    /// @param expected_preview Immutable publication identity expected by the caller.
    /// @return The accepted operation authority.
    [[nodiscard]] persisted_grouped_soa_coordinate_accepted_operation
    finalize_persisted_grouped_soa_coordinate_operation(
        persisted_grouped_soa_coordinate_operation_receipt&& receipt,
        const persisted_grouped_soa_coordinate_operation_receipt_preview&
            expected_preview);
    /// @brief Exercises preclaim failure handling without consuming cache metrics.
    /// @return The computed result.
    [[nodiscard]] static bool
    self_test_persisted_grouped_soa_finalizer_preclaim_contract(
        runtime_metal_archive_pipeline_audit base_audit);
    /// @brief Exercises partial cache-delta accounting with one immutable audit.
    /// @return The computed result.
    [[nodiscard]] static bool self_test_gpu_cache_partial_delta_contract(
        runtime_metal_archive_pipeline_audit base_audit);

    /// @brief Allocates a persistently mapped host-visible Vulkan buffer.
    /// @param bytes Logical byte size.
    /// @param usage Vulkan usage flags.
    /// @return Move-owned host-visible buffer.
    [[nodiscard]] buffer create_host_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    /// @brief Allocates a device buffer with the requested Vulkan usage flags.
    /// @param bytes Logical byte size.
    /// @param usage Vulkan usage flags.
    /// @return Move-owned device buffer.
    [[nodiscard]] buffer create_device_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    /// @brief Returns the physical allocation requirement for a logical buffer size.
    /// @param requested_bytes Logical byte size.
    /// @param usage Vulkan usage flags.
    /// @return Physical device-memory requirement in bytes.
    [[nodiscard]] std::uint64_t buffer_allocation_requirement(
        std::uint64_t requested_bytes,
        VkBufferUsageFlags usage);
    /// @brief Returns the physical allocation requirement for a GPU arena buffer.
    /// @param requested_bytes Logical arena byte size.
    /// @return Physical device-memory requirement in bytes.
    [[nodiscard]] std::uint64_t gpu_arena_allocation_requirement(
        std::uint64_t requested_bytes);
    /// @brief Computes complete arena and output admission requirements for a sketch batch.
    /// @return The computed result.
    [[nodiscard]] gpu_sketch_batch_allocation_requirements
    sketch_batch_allocation_requirements(
        std::uint64_t packed_sequence_bytes,
        std::uint64_t tile_count,
        std::uint32_t minimizer_word_kind,
        std::uint32_t minimizer_words32,
        std::uint64_t max_minimizers);
    /// @brief Releases a runtime-owned Vulkan buffer and clears its identity.
    /// @param allocation Buffer owner to release and invalidate.
    void destroy_buffer(buffer& allocation) noexcept;

    /// @brief Runs one complete Vulkan chaining submission. This records all H2D copies,
    /// the compute dispatch, all D2H copies, and all barriers into one command
    /// buffer lifecycle under one queue lock. Do not call separate public
    /// upload/download helpers from the hot chaining path.
    void run_chaining_tiled_dp_roundtrip(
        const chaining_pipeline_key& key,
        buffer& transfer_scratch,
        buffer& anchors,
        buffer& ranges,
        buffer& best_scores,
        buffer& predecessors,
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

    /// @brief Runs tiled chaining and returns the selected coordinate trace.
    /// @return The computed result.
    [[nodiscard]] gpu_coordinate_chain_trace_result run_chaining_coordinate_trace_roundtrip(
        const chaining_pipeline_key& key,
        buffer& transfer_scratch,
        buffer& anchors,
        buffer& ranges,
        buffer& best_scores,
        buffer& predecessors,
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

    /// @brief Generates packed sequence units entirely on the Vulkan device.
    /// @return Move-owned buffer containing generated packed units.
    buffer run_generate_units_roundtrip(
        std::uint32_t word_bits,
        std::span<const std::uint32_t> cdf,
        generate_units_push push,
        std::uint64_t out_block_bytes);
    /// @brief Downloads one validated byte slice from a host-visible runtime buffer.
    /// @param src Source allocation.
    /// @param src_byte_offset First source byte.
    /// @param dst Destination span receiving the slice.
    void download_slice(
        const buffer& src,
        std::uint64_t src_byte_offset,
        std::span<std::byte> dst);

    /// @brief Looks up seed hits and returns anchors in canonical device-sorted order.
    /// @return Seed lookup result with canonical anchor ordering.
    seed_lookup_result run_seed_lookup_roundtrip(
        runtime_seed_lookup_inputs inputs,
        std::span<const std::uint32_t> prefix_counts,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::uint64_t exact_anchor_count,
        const seed_anchor_sort_plan& sort_plan,
        std::uint64_t expected_vulkan_allocation_bytes);

    /// @brief Returns the per-runtime reusable GPU arena.
    /// @return The computed result.
    [[nodiscard]] gpu_arena& reusable_gpu_arena();

    /// @brief Runs a parallel ordered sketch for one packed sequence.
    /// @return Resident sketch words and exact count.
    runtime_sketch_result run_sketch_roundtrip(
        std::span<const std::byte> packed_sequence,
        const vulkan_rank_lut& rank_lut,
        const vulkan_sketch_params& params,
        std::uint64_t max_minimizers,
        std::span<const std::uint32_t> query_gate_words = {},
        std::uint32_t query_gate_mask = 0u);

    /// @brief Runs an ordered multi-query tile sketch with explicit materialization.
    /// @return Batched sketch buffers, layouts, and dispatch statistics.
    runtime_sketch_batch_result run_sketch_tile_batch_roundtrip(
        std::span<const std::byte> packed_sequence,
        const vulkan_rank_lut& rank_lut,
        const vulkan_sketch_params& params,
        std::span<const gpu_sketch_tile_wire> tiles,
        std::span<const gpu_sketch_query_layout_wire> query_layouts,
        std::uint64_t max_minimizers,
        vulkan_sketch_host_materialization materialization);

    /// @brief Resolves the finite target-index pipeline set before transient admission.
    /// @param params Immutable sketch kernel parameters.
    void prepare_target_index_pipelines(
        const vulkan_sketch_params& params);

    /// @brief Builds a target minimizer index entirely on the Vulkan device.
    /// @return Move-owned GPU target index and build statistics.
    gpu_built_index build_target_index(
        std::span<const std::byte> packed_sequence,
        std::uint64_t padded_sequence_bytes,
        std::uint64_t owned_sequence_staging_bytes,
        std::function<void()> release_owned_sequence_staging,
        const vulkan_rank_lut& rank_lut,
        const vulkan_sketch_params& params,
        std::uint64_t max_target_minimizers,
        std::span<const std::uint32_t> query_gate_words = {},
        std::uint32_t query_gate_mask = 0u,
        gpu_index_gate_identity query_gate_identity = {},
        bioseq::parallelism::memory_permit* peak_permit = nullptr);

    /// @brief Emits per-segment `(kmer, encoded_pos)` pairs and stably sorts both keys.
    /// @return Sorted resident pairs and optional host materialization.
    gpu_sorted_pair_result run_sketch_sort_pairs_roundtrip(
        std::span<const std::byte> packed_sequence,
        const vulkan_rank_lut& rank_lut,
        const vulkan_sketch_params& params,
        std::uint64_t max_target_minimizers);

    /// @brief Orders one contiguous batch of compact sequences on the device.
    /// @return The computed result.
    [[nodiscard]] std::vector<std::uint32_t> run_sequence_sort_roundtrip(
        std::span<const std::byte> packed_units,
        std::span<const sequence_descriptor_wire> descs,
        const sequence_sort_kernel_parameters& params);
    /// @brief Orders a chunked batch of compact sequences on the device.
    /// @return The computed result.
    [[nodiscard]] std::vector<std::uint32_t> run_sequence_sort_roundtrip(
        std::span<const sequence_sort_upload_chunk> chunks,
        std::span<const sequence_descriptor_wire> descs,
        const sequence_sort_kernel_parameters& params);

    /// @brief Runs score/end-point-only banded affine extension for a batch.
    /// @return The computed result.
    [[nodiscard]] std::vector<gpu_alignment_extension_result_wire>
    run_alignment_extension_roundtrip(
        std::span<const std::uint32_t> query_symbols,
        std::span<const std::uint32_t> target_symbols,
        std::span<const gpu_alignment_extension_job_wire> jobs,
        const gpu_alignment_extension_params& params);

    /// @brief Rank-buffer SneakySnake lower-bound batch. Valid unequal-length and
    /// unsupported jobs are returned conservatively with an explicit status;
    /// representable malformed ranges fail open in the shader before loads.
    /// @return The computed result.
    [[nodiscard]] std::vector<
        bioseq::prefilter::prefilter_result>
    run_sneaky_snake_rank_batch_roundtrip(
        std::span<const std::uint8_t> query_ranks,
        std::span<const std::uint8_t> target_ranks,
        std::span<const bioseq::prefilter::prefilter_job> jobs);

    /// @brief Rank-buffer GateKeeper batch. The device treats a mask candidate as a
    /// rejection only after the exact bounded-distance guard confirms it;
    /// representable malformed ranges fail open in the shader before loads.
    /// @return The computed result.
    [[nodiscard]] std::vector<
        bioseq::prefilter::prefilter_result>
    run_gatekeeper_rank_batch_roundtrip(
        std::span<const std::uint8_t> query_ranks,
        std::span<const std::uint8_t> target_ranks,
        std::span<const bioseq::prefilter::prefilter_job> jobs);

    /// @brief Runs CIGAR-capable exact-match extension without a CPU fallback.
    /// @return The computed result.
    [[nodiscard]] alignment_cigar_batch_result
    run_alignment_simple_cigar_roundtrip(
        std::span<const std::uint32_t> query_symbols,
        std::span<const std::uint32_t> target_symbols,
        std::span<const gpu_alignment_cigar_job_wire> jobs,
        const gpu_alignment_cigar_params& params,
        std::uint32_t packed_cigar_capacity);

    /// @brief Returns committed sequence-sort dispatches.
    /// @return The computed result.
    [[nodiscard]] std::uint64_t seq_sort_dispatch_count() const noexcept {
        return seq_sort_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns global full-compare sequence-sort dispatches.
    /// @return The computed result.
    [[nodiscard]] std::uint64_t seq_sort_global_compare_dispatch_count() const noexcept {
        return seq_sort_global_compare_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns dense-scatter sequence-sort dispatches.
    /// @return The computed result.
    [[nodiscard]] std::uint64_t seq_sort_dense_scatter_dispatch_count() const noexcept {
        return seq_sort_dense_scatter_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns bytes uploaded by sequence-sort operations.
    /// @return The computed result.
    [[nodiscard]] std::uint64_t seq_sort_uploaded_bytes_count() const noexcept {
        return seq_sort_uploaded_bytes_.load(std::memory_order_relaxed);
    }
    /// @brief Returns committed alignment-extension dispatches.
    /// @return The computed result.
    [[nodiscard]] std::uint64_t alignment_extension_dispatch_count() const noexcept {
        return alignment_extension_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns committed SneakySnake prefilter dispatches.
    /// @return The computed result.
    [[nodiscard]] std::uint64_t sneaky_snake_dispatch_count() const noexcept {
        return sneaky_snake_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns committed GateKeeper prefilter dispatches.
    /// @return The computed result.
    [[nodiscard]] std::uint64_t gatekeeper_dispatch_count() const noexcept {
        return gatekeeper_dispatch_count_.load(std::memory_order_relaxed);
    }

    /// @brief Counts, emits, and device-sorts seed anchors without a host prepass.
    /// @return Seed lookup result with device-sorted anchors.
    seed_lookup_result run_seed_anchor_roundtrip(
        const gpu_seed_inputs& inputs,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::int32_t max_occ);
    /// @brief Runs seed lookup and tiled chaining for one coordinate result.
    /// @return Coordinate-only result wire.
    gpu_coordinate_result_wire run_seed_to_coordinate_roundtrip(
        const gpu_seed_inputs& inputs,
        std::uint64_t query_len,
        std::uint64_t target_len,
        std::uint32_t kmer_size,
        std::int32_t max_occ,
        const chaining_pipeline_key& chain_key,
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

    /// @brief Runs batched seed-to-coordinate jobs and returns their causal receipt.
    /// @return Coordinate results, dispatch statistics, and operation receipt.
    gpu_seed_coordinate_batch_result run_seed_to_coordinate_batch_roundtrip(
        const gpu_seed_query_inputs& query_inputs,
        std::span<const gpu_seed_coordinate_job> jobs,
        std::uint32_t kmer_size,
        std::int32_t max_occ,
        gpu_seed_coordinate_algorithm coordinate_algorithm,
        const chaining_pipeline_key& chain_key,
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
        device_query_sketch_authorization query_authority = {});

    /// @brief Runs batched seed-to-coordinate jobs and returns only result wires.
    /// @return Coordinate result wires in input-job order.
    std::vector<gpu_coordinate_result_wire> run_seed_to_coordinate_results_roundtrip(
        const gpu_seed_query_inputs& query_inputs,
        std::span<const gpu_seed_coordinate_job> jobs,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::int32_t max_occ,
        gpu_seed_coordinate_algorithm coordinate_algorithm,
        const chaining_pipeline_key& chain_key,
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

    /// @brief Largest power-of-two anchor count the GPU bitonic sort can handle in one
    /// workgroup on this device (bounded by workgroup invocations + shared mem).
    /// Larger anchor spans use the scalable stable GPU radix path.
    /// @return The computed result.
    [[nodiscard]] std::uint32_t gpu_anchor_sort_capacity() const noexcept {
        return gpu_anchor_sort_capacity_;
    }

private:
    [[nodiscard]] static runtime_detail::
        persisted_grouped_soa_coordinate_finalization_plan
    prepare_persisted_grouped_soa_coordinate_finalization(
        const persisted_grouped_soa_coordinate_operation_receipt& receipt,
        const persisted_grouped_soa_coordinate_operation_receipt_preview&
            expected_preview,
        const vulkan_feature_set& live_features,
        bool seed_metrics_claimed) noexcept;

    struct pipeline_cache_admission_scope {
        runtime* owner = nullptr;
        explicit pipeline_cache_admission_scope(runtime& runtime) noexcept
            : owner(&runtime) {}
        pipeline_cache_admission_scope(const pipeline_cache_admission_scope&) =
            delete;
        pipeline_cache_admission_scope& operator=(
            const pipeline_cache_admission_scope&) = delete;
        ~pipeline_cache_admission_scope() {
            if (owner != nullptr) {
                owner->reconcile_pipeline_cache_metadata_locked();
            }
        }
    };

    struct owned_pipeline_handles {
        VkDevice device = VK_NULL_HANDLE;
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;

        owned_pipeline_handles() = default;
        explicit owned_pipeline_handles(VkDevice owner) noexcept
            : device(owner) {}
        owned_pipeline_handles(const owned_pipeline_handles&) = delete;
        owned_pipeline_handles& operator=(const owned_pipeline_handles&) = delete;
        owned_pipeline_handles(owned_pipeline_handles&& other) noexcept
            : device(std::exchange(other.device, VK_NULL_HANDLE)),
              set_layout(std::exchange(
                  other.set_layout, VK_NULL_HANDLE)),
              pipeline_layout(std::exchange(
                  other.pipeline_layout, VK_NULL_HANDLE)),
              descriptor_pool(std::exchange(
                  other.descriptor_pool, VK_NULL_HANDLE)),
              module(std::exchange(other.module, VK_NULL_HANDLE)),
              pipeline(std::exchange(other.pipeline, VK_NULL_HANDLE)) {}
        owned_pipeline_handles& operator=(owned_pipeline_handles&& other) noexcept {
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
        ~owned_pipeline_handles() { reset(); }

        void reset() noexcept;
    };

    /// @brief One cached compute pipeline plus its shader module, keyed by tile/local.
    struct pipeline_entry {
        chaining_pipeline_key key{};
        owned_pipeline_handles handles{};
        std::string entry_name = "main";
        std::unique_ptr<const runtime_metal_archive_pipeline_receipt>
            metal_archive_receipt{};
        std::uint64_t pipeline_create_ns = 0u;

        pipeline_entry() = default;
        pipeline_entry(const pipeline_entry&) = delete;
        pipeline_entry& operator=(const pipeline_entry&) = delete;
        pipeline_entry(pipeline_entry&&) noexcept = default;
        pipeline_entry& operator=(pipeline_entry&&) noexcept = default;
    };

    struct chaining_pipeline {
        VkPipeline pipeline = VK_NULL_HANDLE;
        shader_variant_key shader_key{};
        const runtime_metal_archive_pipeline_receipt* metal_archive_receipt =
            nullptr;
    };

    /// @brief One cached seed-lookup compute pipeline with its own
    /// descriptor-set layout (variable binding count), pipeline layout (variable
    /// push-constant size), descriptor pool, and a single reusable descriptor
    /// set. Keyed by (kind, local_size, max_span) — the only fields that change
    /// the emitted SPIR-V; alphabet/word routing is a runtime push constant.
    struct compute_pipeline {
        shader_kind kind{};
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
        const runtime_metal_archive_pipeline_receipt* metal_archive_receipt =
            nullptr;
    };

    struct owned_compute_pipeline {
        shader_kind kind{};
        std::uint32_t local_size = 0;
        std::uint32_t max_span = 0;
        std::uint32_t binding_count = 0;
        std::uint32_t push_size = 0;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        owned_pipeline_handles handles{};
        std::string entry_name = "main";
        std::unique_ptr<const runtime_metal_archive_pipeline_receipt>
            metal_archive_receipt{};
        std::uint64_t pipeline_create_ns = 0u;

        owned_compute_pipeline() = default;
        owned_compute_pipeline(const owned_compute_pipeline&) = delete;
        owned_compute_pipeline& operator=(const owned_compute_pipeline&) = delete;
        owned_compute_pipeline(owned_compute_pipeline&&) noexcept = default;
        owned_compute_pipeline& operator=(owned_compute_pipeline&&) noexcept =
            default;

        [[nodiscard]] compute_pipeline view() const noexcept {
            return compute_pipeline{
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
    compute_pipeline get_compute_pipeline_locked(
        shader_kind kind, std::uint32_t local_size, std::uint32_t max_span);
    void prepare_target_index_pipelines_locked(
        const vulkan_sketch_params& params);
    void bind_compute_pipeline_descriptors_locked(
        const compute_pipeline& entry,
        std::span<const VkBuffer> buffers);
    // GPU frontend — bind storage descriptors as explicit (buffer, offset,
    // range) so arena-backed slices that share a physical buffer bind correctly.
    void bind_descriptors_offsets_locked(
        const compute_pipeline& entry,
        std::span<const VkDescriptorBufferInfo> infos);

    // GPU frontend — fresh-descriptor-set-per-dispatch pool. Lets one command
    // buffer record many dispatches of the same pipeline with different buffer
    // bindings (ping-pong sort passes) without re-updating a single set in
    // flight. Reset once per frontend op.
    void ensure_dispatch_descriptor_pool_locked();
    void reset_dispatch_descriptor_pool_locked();
    [[nodiscard]] VkDescriptorSet allocate_dispatch_descriptor_set_locked(VkDescriptorSetLayout layout);
    void record_compute_dispatch_locked(
        VkCommandBuffer cmd, const compute_pipeline& pipe,
        std::span<const VkDescriptorBufferInfo> infos,
        const void* push, std::uint32_t push_size, std::uint32_t group_count,
        runtime_pipeline_use_collector* collector = nullptr,
        std::size_t collector_job_index = 0u);
    // record one full device radix sort (all passes) into an open command
    // buffer. Sorts the key/value arena slices in place via ping-pong scratch.
    void record_radix_sort_locked(
        VkCommandBuffer cmd,
        const gpu_arena_slice& keys, const gpu_arena_slice& values,
        const gpu_arena_slice& keys_scratch, const gpu_arena_slice& values_scratch,
        const gpu_arena_slice& histogram,
        const gpu_sort_plan& plan, std::uint32_t num_workgroups,
        std::uint32_t elems_per_wg, std::uint32_t value_words,
        bool& result_in_scratch,
        runtime_pipeline_use_collector* collector = nullptr,
        std::size_t collector_job_index = 0u);
    // Two-key stable sort: radix by value (encoded_pos) then
    // radix by key (kmer). Because the second sort is stable the result is
    // (kmer, encoded_pos) ordered.
    void record_stable_pair_sort_by_value_then_key_locked(
        VkCommandBuffer command_buffer,
        sorted_pair_device_views& views,
        gpu_arena_slice hist,
        gpu_sort_plan value_sort_plan,
        gpu_sort_plan key_sort_plan,
        std::uint32_t num_workgroups,
        std::uint32_t elems_per_workgroup);

    void init();
    void shutdown() noexcept;
    void resolve_moltenvk_private_api() noexcept;
    [[nodiscard]] bool should_use_shader_provider_locked() const noexcept;
    std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    [[nodiscard]] std::uint64_t buffer_memory_requirement_locked(
        std::uint64_t bytes, VkBufferUsageFlags usage) const;
    [[nodiscard]] std::uint64_t
    prefilter_workspace_allocation_bytes_locked() const noexcept;
    void ensure_prefilter_workspace_buffer_locked(
        buffer& allocation,
        std::uint64_t bytes,
        VkBufferUsageFlags usage);
    void release_prefilter_workspace_locked() noexcept;
    [[nodiscard]] std::vector<
        bioseq::prefilter::prefilter_result>
    run_rank_prefilter_batch_roundtrip_locked(
        shader_kind kind,
        std::uint32_t push_mode,
        std::size_t replay_index,
        std::span<const std::uint8_t> query_ranks,
        std::span<const std::uint8_t> target_ranks,
        std::span<const bioseq::prefilter::prefilter_job> jobs);
    [[nodiscard]] std::uint64_t pipeline_cache_metadata_bytes_locked()
        const noexcept;
    void admit_pipeline_cache_growth_locked(
        std::size_t normal_capacity,
        std::size_t compute_pipeline_capacity);
    void reconcile_pipeline_cache_metadata_locked() noexcept;
    chaining_pipeline create_pipeline_locked(const chaining_pipeline_key& key);
    void upload_locked(buffer& transfer_scratch, buffer& dst, std::span<const std::byte> bytes,
                       VkAccessFlags dst_access);
    void download_locked(buffer& transfer_scratch, const buffer& src, std::span<std::byte> bytes);
    [[nodiscard]] std::uint64_t next_operation_identity_locked() {
        constexpr std::uint64_t exhausted =
            (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t identity =
            operation_token_identity_.load(std::memory_order_relaxed);
        while (identity != exhausted) {
            if (operation_token_identity_.compare_exchange_weak(
                    identity,
                    identity + 1u,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return identity;
            }
        }
        throw vulkan_runtime_error(
            "Vulkan operation receipt identity space exhausted");
    }

    struct prefilter_recorded_command {
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        shader_kind kind{};
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        gpu_prefilter_push push{};
        std::uint32_t groups = 0u;
        bool recorded = false;
    };

    struct prefilter_descriptor_binding_cache {
        std::array<VkBuffer, 4> buffers{};
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    };

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    // Two SneakySnake routes, one generic GateKeeper route, and four
    // homogeneous LxE GateKeeper production routes each retain an independent
    // replay identity so interleaved benchmark/production batches do not
    // re-record command buffers when their specialization changes.
    std::array<prefilter_recorded_command, k_rank_prefilter_replay_route_count>
        prefilter_recorded_commands_{};
    VkFence fence_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    // Declared before both caches so cache-owned strings/receipts/entries are
    // destroyed before their aggregate metadata reservation is returned.
    bioseq::parallelism::memory_permit pipeline_cache_metadata_permit_;
    std::vector<pipeline_entry> pipeline_cache_;
    std::vector<owned_compute_pipeline> compute_pipelines_;
    bool seed_metrics_claimed_ = false;
    std::uint32_t gpu_anchor_sort_capacity_ = 0;
    std::optional<gpu_arena> gpu_arena_;  // Lazily created.
    // High-water mapped workspace shared by the synchronous rank-prefilter
    // APIs. Queue access is serialized by mu_, so one runtime-owned set is
    // sufficient for both algorithms and all seven fixed replay routes.
    // Declared before the buffers so reverse destruction order releases every
    // admitted allocation before returning its reservation.
    bioseq::parallelism::memory_permit prefilter_workspace_permit_;
    buffer prefilter_query_buffer_;
    buffer prefilter_target_buffer_;
    buffer prefilter_jobs_buffer_;
    buffer prefilter_results_buffer_;
    // Index 0 owns SneakySnake's descriptor state (both replay modes); index 1
    // owns GateKeeper's. Alternating algorithms therefore does not make the
    // next call pay for an unchanged descriptor-set update.
    std::array<prefilter_descriptor_binding_cache, 2>
        prefilter_descriptor_binding_cache_{};
    VkDescriptorPool dispatch_descriptor_pool_ = VK_NULL_HANDLE;  // Fresh-set pool.
    using set_workgroup_size_mvk_function =
        void (VKAPI_PTR*)(VkShaderModule module, std::uint32_t x, std::uint32_t y, std::uint32_t z);
    set_workgroup_size_mvk_function set_workgroup_size_mvk_ = nullptr;
    bool metallib_provider_capable_ = false;

    vulkan_feature_set features_{};
    std::string init_error_{};
    std::atomic<std::uint64_t> chaining_dispatch_count_{0};
    std::atomic<std::uint64_t> generate_units_dispatch_count_{0};
    std::atomic<std::uint64_t> seq_sort_dispatch_count_{0};
    std::atomic<std::uint64_t> seq_sort_global_compare_dispatch_count_{0};
    std::atomic<std::uint64_t> seq_sort_dense_scatter_dispatch_count_{0};
    std::atomic<std::uint64_t> seq_sort_uploaded_bytes_{0};  // Full-payload receipt
    std::atomic<std::uint64_t> alignment_extension_dispatch_count_{0};
    std::atomic<std::uint64_t> sneaky_snake_dispatch_count_{0};
    std::atomic<std::uint64_t> gatekeeper_dispatch_count_{0};
    std::atomic<std::uint64_t> operation_token_identity_{1};
    std::mutex mu_;
};

[[nodiscard]] inline bool hardware_device_available(const runtime& runtime) noexcept {
    return runtime.is_usable() && !runtime.features().software_device;
}

inline void require_hardware_device(const runtime& runtime, const char* operation) {
    if (!runtime.is_usable()) {
        throw vulkan_runtime_error(fmt::format(
            "{} selected but Vulkan runtime is unavailable: {}",
            operation,
            runtime.initialization_error()));
    }
    if (runtime.features().software_device) {
        throw vulkan_runtime_error(fmt::format(
            "{} selected but active Vulkan device '{}' is a CPU/software device "
            "(VkPhysicalDeviceType={}); explicit GPU execution requires a hardware Vulkan device",
            operation,
            runtime.features().device_name.data(),
            runtime.features().device_type));
    }
}

[[nodiscard]] inline std::uint64_t seed_anchor_reserved_bytes(
    std::uint64_t anchor_count,
    std::uint64_t workgroup_count) noexcept
{
    using bioseq::common::saturating_mul;
    const std::uint64_t anchors = saturating_mul(anchor_count, 24u);
    const std::uint64_t keys = saturating_mul(anchor_count, 8u);
    const std::uint64_t anchor_scratch = anchors;
    const std::uint64_t key_scratch = keys;
    const std::uint64_t histogram =
        saturating_mul(saturating_mul(workgroup_count, 256u), 4u);
    const std::uint64_t host_download = anchors;
    std::uint64_t total = 0;
    total = bioseq::common::saturating_add(total, anchors);
    total = bioseq::common::saturating_add(total, keys);
    total = bioseq::common::saturating_add(total, anchor_scratch);
    total = bioseq::common::saturating_add(total, key_scratch);
    total = bioseq::common::saturating_add(total, histogram);
    total = bioseq::common::saturating_add(total, host_download);
    return total;
}

} // namespace bioseq::gpu::vulkan

// ============================================================================
// Implementation
// ============================================================================

namespace bioseq::gpu::vulkan {

namespace {

#ifdef BIOSEQ_TEST_INDEXES
struct vulkan_submission_test_state {
  std::atomic<std::uint8_t> fault{
      static_cast<std::uint8_t>(vulkan_submission_test_fault::none)};
  std::atomic<std::uint8_t> release_gate{1u};
  std::atomic<std::uint8_t> delivery_gate{1u};
  std::atomic<std::uint8_t> waiting_after_submit{0u};
  std::atomic<std::uint8_t> waiting_after_drain{0u};
  std::atomic<std::uint8_t> cancellation_requested{0u};
  std::atomic<std::uint8_t> upload_failure_latched{0u};
  std::atomic<std::uint64_t> upload_latches{0u};
  std::atomic<std::uint64_t> submissions{0u};
  std::atomic<std::uint64_t> fence_drains{0u};
  std::atomic<std::uint64_t> delivered_faults{0u};
};

vulkan_submission_test_state g_vulkan_submission_test_state{};

[[nodiscard]] vulkan_submission_test_fault
active_vulkan_submission_test_fault() noexcept {
  return static_cast<vulkan_submission_test_fault>(
      g_vulkan_submission_test_state.fault.load(std::memory_order_acquire));
}

void note_vulkan_submission_test_upload() noexcept {
  const auto fault = active_vulkan_submission_test_fault();
  if (fault == vulkan_submission_test_fault::none) {
    return;
  }
  g_vulkan_submission_test_state.upload_latches.fetch_add(
      1u, std::memory_order_relaxed);
  if (fault == vulkan_submission_test_fault::upload_failure_after_submit) {
    g_vulkan_submission_test_state.upload_failure_latched.store(
        1u, std::memory_order_release);
  }
}

[[nodiscard]] vulkan_submission_test_fault
delay_vulkan_submission_test_after_submit() noexcept {
  const auto fault = active_vulkan_submission_test_fault();
  if (fault == vulkan_submission_test_fault::none) {
    return fault;
  }
  g_vulkan_submission_test_state.submissions.fetch_add(
      1u, std::memory_order_relaxed);
  g_vulkan_submission_test_state.waiting_after_submit.store(
      1u, std::memory_order_release);
  std::uint8_t released = g_vulkan_submission_test_state.release_gate.load(
      std::memory_order_acquire);
  while (released == 0u) {
    g_vulkan_submission_test_state.release_gate.wait(0u,
                                                     std::memory_order_acquire);
    released = g_vulkan_submission_test_state.release_gate.load(
        std::memory_order_acquire);
  }
  g_vulkan_submission_test_state.waiting_after_submit.store(
      0u, std::memory_order_release);
  return fault;
}

void delay_vulkan_submission_test_after_drain(
    vulkan_submission_test_fault fault) noexcept {
  if (fault == vulkan_submission_test_fault::none) {
    return;
  }
  auto &state = g_vulkan_submission_test_state;
  state.fence_drains.fetch_add(1u, std::memory_order_release);
  state.waiting_after_drain.store(1u, std::memory_order_release);
  std::uint8_t released =
      state.delivery_gate.load(std::memory_order_acquire);
  while (released == 0u) {
    state.delivery_gate.wait(0u, std::memory_order_acquire);
    released = state.delivery_gate.load(std::memory_order_acquire);
  }
  state.waiting_after_drain.store(0u, std::memory_order_release);
}

void deliver_vulkan_submission_test_fault_after_drain(
    vulkan_submission_test_fault fault) {
  if (fault == vulkan_submission_test_fault::none) {
    return;
  }
  g_vulkan_submission_test_state.delivered_faults.fetch_add(
      1u, std::memory_order_release);
  g_vulkan_submission_test_state.fault.store(
      static_cast<std::uint8_t>(vulkan_submission_test_fault::none),
      std::memory_order_release);
  if (fault == vulkan_submission_test_fault::cancellation_after_submit) {
    throw vulkan_runtime_error("injected Vulkan cancellation after fence drain");
  }
  if (g_vulkan_submission_test_state.upload_failure_latched.load(
          std::memory_order_acquire) != 0u) {
    throw vulkan_runtime_error(
        "injected Vulkan upload failure after fence drain");
  }
  throw vulkan_runtime_error(
      "injected Vulkan upload failure was not latched before submit");
}
#endif

/// @brief Throw `vulkan_runtime_error` on any non-success VkResult. A free function
/// rather than a capture-all lambda so it leaves no per-call closure in the hot
/// path and is trivially inlinable.
inline void check_vk(VkResult result, const char *what) {
  if (result != VK_SUCCESS) {
    throw vulkan_runtime_error(
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

constexpr const char* k_portability_enumeration = "VK_KHR_portability_enumeration";
constexpr const char* k_get_physical_device_properties2 = "VK_KHR_get_physical_device_properties2";
constexpr const char* k_portability_subset = "VK_KHR_portability_subset";
constexpr const char* k_layer_settings = VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;

/// @brief True if `name` appears in a list of VkExtensionProperties.
/// @return The computed result.
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

/// @brief The vcpkg Vulkan loader used by the bench/test binaries does not search
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
                bioseq_log<logging_level::debug>("[vulkan_runtime] using MoltenVK ICD manifest {}", path);
            }
            return;
        }
    }
#endif
}

struct runtime_metal_archive_configuration_slot {
    std::mutex mu;
    std::shared_ptr<const runtime_metal_archive_configuration_image> configuration;
    bool sealed = false;
    std::atomic<bool> installed_accepted{false};
    std::atomic<std::uint64_t> install_validation_count{0u};
    std::atomic<std::uint64_t> live_gate_calls{0u};
    std::atomic<std::uint64_t> live_configuration_scan_count{0u};
};

runtime_metal_archive_configuration_slot&
runtime_metal_archive_configuration_slot_instance() noexcept {
    static runtime_metal_archive_configuration_slot slot;
    return slot;
}

[[nodiscard]] std::shared_ptr<const runtime_metal_archive_configuration_image>
runtime_metal_archive_configuration_snapshot() {
    auto& slot = runtime_metal_archive_configuration_slot_instance();
    std::lock_guard<std::mutex> guard(slot.mu);
    return slot.configuration;
}

} // namespace

#ifdef BIOSEQ_TEST_INDEXES
void arm_vulkan_submission_test_seam(vulkan_submission_test_fault fault) noexcept {
  auto &state = g_vulkan_submission_test_state;
  state.release_gate.store(0u, std::memory_order_relaxed);
  state.delivery_gate.store(0u, std::memory_order_relaxed);
  state.waiting_after_submit.store(0u, std::memory_order_relaxed);
  state.waiting_after_drain.store(0u, std::memory_order_relaxed);
  state.cancellation_requested.store(0u, std::memory_order_relaxed);
  state.upload_failure_latched.store(0u, std::memory_order_relaxed);
  state.upload_latches.store(0u, std::memory_order_relaxed);
  state.submissions.store(0u, std::memory_order_relaxed);
  state.fence_drains.store(0u, std::memory_order_relaxed);
  state.delivered_faults.store(0u, std::memory_order_relaxed);
  state.fault.store(static_cast<std::uint8_t>(fault),
                    std::memory_order_release);
}

void release_vulkan_submission_test_fence() noexcept {
  auto &state = g_vulkan_submission_test_state;
  if (active_vulkan_submission_test_fault() ==
      vulkan_submission_test_fault::cancellation_after_submit) {
    state.cancellation_requested.store(1u, std::memory_order_release);
  }
  state.release_gate.store(1u, std::memory_order_release);
  state.release_gate.notify_all();
}

void release_vulkan_submission_test_delivery() noexcept {
  auto &state = g_vulkan_submission_test_state;
  state.delivery_gate.store(1u, std::memory_order_release);
  state.delivery_gate.notify_all();
}

void reset_vulkan_submission_test_seam() noexcept {
  auto &state = g_vulkan_submission_test_state;
  state.fault.store(static_cast<std::uint8_t>(vulkan_submission_test_fault::none),
                    std::memory_order_release);
  state.release_gate.store(1u, std::memory_order_release);
  state.release_gate.notify_all();
  state.delivery_gate.store(1u, std::memory_order_release);
  state.delivery_gate.notify_all();
  state.waiting_after_submit.store(0u, std::memory_order_relaxed);
  state.waiting_after_drain.store(0u, std::memory_order_relaxed);
  state.cancellation_requested.store(0u, std::memory_order_relaxed);
  state.upload_failure_latched.store(0u, std::memory_order_relaxed);
  state.upload_latches.store(0u, std::memory_order_relaxed);
  state.submissions.store(0u, std::memory_order_relaxed);
  state.fence_drains.store(0u, std::memory_order_relaxed);
  state.delivered_faults.store(0u, std::memory_order_relaxed);
}

[[nodiscard]] vulkan_submission_test_snapshot
capture_vulkan_submission_test_snapshot() noexcept {
  auto &state = g_vulkan_submission_test_state;
  return {
      .fault = active_vulkan_submission_test_fault(),
      .upload_latches = state.upload_latches.load(std::memory_order_acquire),
      .submissions = state.submissions.load(std::memory_order_acquire),
      .fence_drains = state.fence_drains.load(std::memory_order_acquire),
      .delivered_faults =
          state.delivered_faults.load(std::memory_order_acquire),
      .waiting_after_submit =
          state.waiting_after_submit.load(std::memory_order_acquire) != 0u,
      .waiting_after_drain =
          state.waiting_after_drain.load(std::memory_order_acquire) != 0u,
      .cancellation_requested =
          state.cancellation_requested.load(std::memory_order_acquire) != 0u,
      .upload_failure_latched =
          state.upload_failure_latched.load(std::memory_order_acquire) != 0u,
  };
}
#endif

void runtime::owned_pipeline_handles::reset() noexcept {
  if (device != VK_NULL_HANDLE) {
    if (pipeline != VK_NULL_HANDLE && vkDestroyPipeline != nullptr) {
      vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (module != VK_NULL_HANDLE && vkDestroyShaderModule != nullptr) {
      vkDestroyShaderModule(device, module, nullptr);
    }
    if (descriptor_pool != VK_NULL_HANDLE &&
        vkDestroyDescriptorPool != nullptr) {
      vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    }
    if (pipeline_layout != VK_NULL_HANDLE &&
        vkDestroyPipelineLayout != nullptr) {
      vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    }
    if (set_layout != VK_NULL_HANDLE &&
        vkDestroyDescriptorSetLayout != nullptr) {
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

std::expected<std::shared_ptr<const runtime_metal_archive_configuration_image>,
              std::string>
make_runtime_metal_archive_configuration_image(
    std::shared_ptr<const sealed_shader_cache_seed_image> seed_image,
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid,
    std::uint64_t metal_device_registry_id) {
    if (!seed_image || !seed_image->is_accepted()) {
        return std::unexpected(
            "runtime Metal archive configuration has no accepted seed image");
    }
    std::uint64_t archive_count = 0u;
    for (const sealed_shader_cache_artifact_image& artifact :
         seed_image->artifacts()) {
        archive_count += artifact.descriptor.kind
                == sealed_shader_cache_artifact_kind::metal_binary_archive
            ? 1u : 0u;
    }
    if (archive_count == 0u
        || archive_count
            > sealed_shader_cache_detail::k_maximum_seed_image_artifacts) {
        return std::unexpected(
            "runtime Metal archive configuration archive count is invalid");
    }
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    std::uint64_t retained = sizeof(runtime_metal_archive_configuration_image);
    retained = saturating_add(
        saturating_add(
            retained,
            saturating_mul(
                archive_count,
                sizeof(runtime_metal_archive_spec)
                    + 2u * (sealed_shader_cache_detail::
                                k_maximum_manifest_filename_bytes
                            + 1024u))),
        2u * 1024u);
    const std::uint64_t peak = saturating_mul(
        retained, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (retained == (std::numeric_limits<std::uint64_t>::max)()
        || peak == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(
            "runtime Metal archive configuration admission saturated");
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = bioseq::parallelism::memory_admission_controller::instance()
        .try_reserve(peak, &admission_failure);
    if (!permit) {
        return std::unexpected(shader_cache_message(
            "runtime Metal archive configuration admission denied before allocation: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(
                admission_failure.reason),
            " requested=", admission_failure.requested_bytes));
    }
    try {
        runtime_metal_archive_configuration configuration{};
        configuration.seed_image = std::move(seed_image);
        configuration.vulkan_device_uuid = vulkan_device_uuid;
        configuration.metal_device_registry_id = metal_device_registry_id;
        configuration.archives.reserve(
            static_cast<std::size_t>(archive_count));
        std::optional<metal_archive_negative_error_fingerprint> common_error;
        for (std::size_t ordinal = 0u;
             ordinal < configuration.seed_image->artifacts().size();
             ++ordinal) {
            const sealed_shader_cache_artifact_image& artifact =
                configuration.seed_image->artifacts()[ordinal];
            if (artifact.descriptor.kind
                != sealed_shader_cache_artifact_kind::metal_binary_archive) {
                continue;
            }
            const std::string qualification_filename =
                artifact.descriptor.filename + ".qualification";
            const auto* qualification_image =
                sealed_shader_cache_detail::find_artifact_image(
                    configuration.seed_image->artifacts(),
                    sealed_shader_cache_artifact_kind::
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
                || !qualification->is_production_accepted()
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
                configuration.qualification_wire_revision =
                    qualification->wire_revision;
                configuration.helper_wire_revision =
                    qualification->helper_wire_revision;
                configuration.helper_build_identity =
                    qualification->helper_build_identity;
                configuration.helper_source = qualification->helper_source;
                configuration.helper_executable_size =
                    qualification->helper_executable_size;
                configuration.helper_executable_sha256 =
                    qualification->helper_executable_sha256;
                configuration.helper_metal_registry_id =
                    qualification->helper_metal_registry_id;
            } else if (configuration.qualification_wire_revision
                           != qualification->wire_revision
                || configuration.helper_wire_revision
                    != qualification->helper_wire_revision
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
            configuration.archives.push_back(runtime_metal_archive_spec{
                .artifact_ordinal = static_cast<std::uint32_t>(ordinal),
                .artifact_filename = artifact.descriptor.filename,
                .size = artifact.descriptor.size,
                .sha256 = artifact.descriptor.sha256,
                .negative_error_domain =
                    std::move(qualification->negative_error.domain),
                .negative_error_code = qualification->negative_error.code,
            });
        }
        if (!configuration.is_valid()) {
            return std::unexpected(
                "runtime Metal archive configuration factory produced invalid evidence");
        }
        auto installed = std::shared_ptr<runtime_metal_archive_configuration_image>(
            new runtime_metal_archive_configuration_image(
                std::move(configuration), peak, std::move(*permit)));
        if (!installed->reconcile() || !installed->is_valid()) {
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
    std::shared_ptr<const runtime_metal_archive_configuration_image> configuration) {
    if (!configuration || !configuration->is_valid()) {
        return std::unexpected(
            "runtime Metal archive configuration is incomplete or non-canonical");
    }
    auto& slot = runtime_metal_archive_configuration_slot_instance();
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

bool is_runtime_metal_archive_configuration_installed() noexcept {
    auto& slot = runtime_metal_archive_configuration_slot_instance();
    return slot.installed_accepted.load(std::memory_order_acquire);
}

[[nodiscard]] runtime_metal_archive_live_gate_result
capture_runtime_metal_archive_live_gate_result() noexcept {
    auto& slot = runtime_metal_archive_configuration_slot_instance();
    return runtime_metal_archive_live_gate_result{
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
struct objc_runtime_functions {
    void* msg_send = nullptr;
    void* (*get_class)(const char*) = nullptr;
    void* (*sel_register_name)(const char*) = nullptr;
    void* (*object_get_class)(void*) = nullptr;
    void* (*class_get_superclass)(void*) = nullptr;
    void* (*class_get_instance_method)(void*, void*) = nullptr;
    void* (*method_get_implementation)(void*) = nullptr;
    void* (*method_set_implementation)(void*, void*) = nullptr;
};

struct moltenvk_command_library_interpose {
    objc_runtime_functions objc{};
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

moltenvk_command_library_interpose& moltenvk_command_interpose_state() noexcept {
    static moltenvk_command_library_interpose state;
    return state;
}

template <class p_function_type>
[[nodiscard]] p_function_type objc_msg_cast(void* function) noexcept {
    return reinterpret_cast<p_function_type>(function);
}

class scoped_objc_release {
public:
    scoped_objc_release() = default;
    scoped_objc_release(objc_runtime_functions* objc, void* release_selector,
                      void* object) noexcept
        : objc_(objc), release_selector_(release_selector), object_(object) {}
    scoped_objc_release(const scoped_objc_release&) = delete;
    scoped_objc_release& operator=(const scoped_objc_release&) = delete;
    scoped_objc_release(scoped_objc_release&& other) noexcept
        : objc_(std::exchange(other.objc_, nullptr)),
          release_selector_(std::exchange(other.release_selector_, nullptr)),
          object_(std::exchange(other.object_, nullptr)) {}
    scoped_objc_release& operator=(scoped_objc_release&&) = delete;
    ~scoped_objc_release() { reset(); }

    [[nodiscard]] void* get() const noexcept { return object_; }
    [[nodiscard]] void* release() noexcept {
        return std::exchange(object_, nullptr);
    }
    void reset() noexcept {
        if (objc_ != nullptr && objc_->msg_send != nullptr
            && release_selector_ != nullptr && object_ != nullptr) {
            using release_function = void (*)(void*, void*);
            objc_msg_cast<release_function>(objc_->msg_send)(
                object_, release_selector_);
        }
        object_ = nullptr;
    }

private:
    objc_runtime_functions* objc_ = nullptr;
    void* release_selector_ = nullptr;
    void* object_ = nullptr;
};

extern "C" void* bioseq_moltenvk_new_library_with_source(
    void* self, void* cmd, void* source, void* options, void** error)
{
    auto& state = moltenvk_command_interpose_state();
    using utf8_function = const char* (*)(void*, void*);
    const char* text = nullptr;
    if (source != nullptr && state.sel_utf8_string != nullptr) {
        text = objc_msg_cast<utf8_function>(state.objc.msg_send)(source, state.sel_utf8_string);
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
        using library_data_function = void* (*)(void*, void*, void*, void**);
        dispatch_data_t mtl_data = dispatch_data_create(
            state.metallib.data(),
            state.metallib.size(),
            nullptr,
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (mtl_data != nullptr) {
            if (void* library = objc_msg_cast<library_data_function>(state.objc.msg_send)(
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
    using source_function = void* (*)(void*, void*, void*, void*, void**);
    return objc_msg_cast<source_function>(state.original_new_library_with_source)(
        self, cmd, source, options, error);
}

struct objc_block_literal {
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
    auto* block = static_cast<objc_block_literal*>(completion);
    if (block->invoke == nullptr) {
        return;
    }
    using invoke_function = void (*)(void*, void*, void*);
    objc_msg_cast<invoke_function>(block->invoke)(completion, library, error);
}

void invoke_metal_pipeline_completion(
    void* completion,
    void* pipeline,
    void* reflection,
    void* error) noexcept {
    if (completion == nullptr) return;
    auto* block = static_cast<objc_block_literal*>(completion);
    if (block->invoke == nullptr) return;
    using invoke_function = void (*)(void*, void*, void*, void*);
    objc_msg_cast<invoke_function>(block->invoke)(
        completion, pipeline, reflection, error);
}

extern "C" void bioseq_moltenvk_new_library_with_source_async(
    void* self, void* cmd, void* source, void* options, void* completion)
{
    auto& state = moltenvk_command_interpose_state();
    using utf8_function = const char* (*)(void*, void*);
    const char* text = nullptr;
    if (source != nullptr && state.sel_utf8_string != nullptr) {
        text = objc_msg_cast<utf8_function>(state.objc.msg_send)(source, state.sel_utf8_string);
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
        using library_data_function = void* (*)(void*, void*, void*, void**);
        dispatch_data_t mtl_data = dispatch_data_create(
            state.metallib.data(),
            state.metallib.size(),
            nullptr,
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (mtl_data != nullptr) {
            void* error = nullptr;
            void* library = objc_msg_cast<library_data_function>(state.objc.msg_send)(
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
    using source_async_function = void (*)(void*, void*, void*, void*, void*);
    objc_msg_cast<source_async_function>(state.original_new_library_with_source_async)(
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
        is_runtime_metal_archive_configuration_installed();
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
        bioseq_log<logging_level::debug>("[vulkan_runtime] MoltenVK command metallib unavailable at {}", path);
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

    objc_runtime_functions objc{};
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

    using create_device_function = void* (*)();
    auto create_device = reinterpret_cast<create_device_function>(
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
    scoped_objc_release device_owner{
        &objc, release_selector, device};

    void* sel_source = objc.sel_register_name("newLibraryWithSource:options:error:");
    void* sel_source_async =
        objc.sel_register_name("newLibraryWithSource:options:completionHandler:");
    state.sel_new_library_with_source = sel_source;
    state.sel_new_library_with_source_async = sel_source_async;
    state.sel_utf8_string = objc.sel_register_name("UTF8String");
    state.sel_new_library_with_data_error = objc.sel_register_name("newLibraryWithData:error:");
    using registry_function = unsigned long long (*)(void*, void*);
    state.loader_registry_id = static_cast<std::uint64_t>(
        objc_msg_cast<registry_function>(objc.msg_send)(
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
            bioseq_log<logging_level::debug>(
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

struct command_interpose_device_binding {
    std::uint64_t registry_id = 0u;
    std::uint64_t methods_present = 0u;
    std::uint64_t wrapper_methods = 0u;
};

/// @brief Rare PSO-creation receipt: bind the receipt to the exact intercepted device
/// and its live method table. Submit/wait gates consume cached scalar evidence;
/// this two-selector reflection runs only while creating a pipeline receipt.
/// @return The computed result.
[[nodiscard]] command_interpose_device_binding
capture_command_interpose_device_binding(void* device) noexcept {
    auto& state = moltenvk_command_interpose_state();
    command_interpose_device_binding result{};
    if (device == nullptr || !state.installed
        || state.objc.msg_send == nullptr
        || state.objc.object_get_class == nullptr
        || state.objc.class_get_instance_method == nullptr
        || state.objc.class_get_superclass == nullptr
        || state.objc.method_get_implementation == nullptr) {
        return result;
    }
    using registry_function = unsigned long long (*)(void*, void*);
    result.registry_id = static_cast<std::uint64_t>(
        objc_msg_cast<registry_function>(state.objc.msg_send)(
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

struct active_metal_archive_pipeline_scope;

struct metal_pipeline_archive_interpose {
    objc_runtime_functions objc{};
    std::shared_ptr<const runtime_metal_archive_configuration_image>
        configuration_image;
    std::array<std::uint8_t, VK_UUID_SIZE> runtime_vulkan_device_uuid{};
    runtime_vulkan_device_identity runtime_vulkan_device_identity{};
    std::uint64_t runtime_metal_registry_id = 0u;
    runtime_metal_archive_set_identity configured_identity{};
    runtime_metal_archive_set_identity loaded_identity{};
    // Declared before retained Objective-C archive ownership so teardown
    // releases the NSArray/archives before returning their residency budget.
    bioseq::parallelism::memory_permit archive_residency_permit;
    std::uint64_t archive_residency_peak_bytes = 0u;
    std::uint64_t archive_residency_retained_bytes = 0u;
    void* loader_device = nullptr;
    void* archive_array = nullptr;
    runtime_metal_archive_process_qualification process_qualification{};
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
    std::shared_ptr<active_metal_archive_pipeline_scope> active_scope;
    std::atomic<std::uint64_t> outside_scope_interceptions{0u};

    ~metal_pipeline_archive_interpose() {
        if (sel_release != nullptr && objc.msg_send != nullptr) {
            using release_function = void (*)(void*, void*);
            if (archive_array != nullptr) {
                objc_msg_cast<release_function>(objc.msg_send)(
                    archive_array, sel_release);
                archive_array = nullptr;
            }
            if (loader_device != nullptr) {
                objc_msg_cast<release_function>(objc.msg_send)(
                    loader_device, sel_release);
                loader_device = nullptr;
            }
        }
    }
};

metal_pipeline_archive_interpose& metal_pipeline_archive_interpose_state() noexcept {
    static metal_pipeline_archive_interpose state;
    return state;
}

void rollback_metal_archive_loaded_state(
    metal_pipeline_archive_interpose& state) noexcept {
    if (state.archive_array != nullptr && state.sel_release != nullptr
        && state.objc.msg_send != nullptr) {
        using release_function = void (*)(void*, void*);
        objc_msg_cast<release_function>(state.objc.msg_send)(
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
    metal_pipeline_archive_interpose& state) noexcept {
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
        using release_function = void (*)(void*, void*);
        objc_msg_cast<release_function>(state.objc.msg_send)(
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
    objc_runtime_functions& objc,
    void* ns_string,
    void* sel_utf8_string)
{
    if (ns_string == nullptr || sel_utf8_string == nullptr) {
        return {};
    }
    using utf8_function = const char* (*)(void*, void*);
    const char* text = objc_msg_cast<utf8_function>(objc.msg_send)(ns_string, sel_utf8_string);
    return text == nullptr ? std::string{} : std::string(text);
}

[[nodiscard]] void* bioseq_make_ns_string(objc_runtime_functions& objc, std::string_view value)
{
    void* ns_string_cls = objc.get_class("NSString");
    if (ns_string_cls == nullptr) {
        return nullptr;
    }
    std::string owned(value);
    using string_function = void* (*)(void*, void*, const char*);
    return objc_msg_cast<string_function>(objc.msg_send)(
        ns_string_cls,
        objc.sel_register_name("stringWithUTF8String:"),
        owned.c_str());
}

[[nodiscard]] void* bioseq_make_file_url(objc_runtime_functions& objc, const std::filesystem::path& path)
{
    void* url_cls = objc.get_class("NSURL");
    void* ns_path = bioseq_make_ns_string(objc, path.string());
    if (url_cls == nullptr || ns_path == nullptr) {
        return nullptr;
    }
    using url_function = void* (*)(void*, void*, void*);
    return objc_msg_cast<url_function>(objc.msg_send)(
        url_cls,
        objc.sel_register_name("fileURLWithPath:"),
        ns_path);
}

[[nodiscard]] std::string bioseq_ns_error_description(objc_runtime_functions& objc, void* error)
{
    if (error == nullptr) {
        return {};
    }
    using get_object_function = void* (*)(void*, void*);
    void* desc = objc_msg_cast<get_object_function>(objc.msg_send)(
        error,
        objc.sel_register_name("localizedDescription"));
    return bioseq_ns_string_utf8(objc, desc, objc.sel_register_name("UTF8String"));
}

void mark_metal_archive_sticky_failure(
    metal_pipeline_archive_interpose& state,
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
    metal_pipeline_archive_interpose& state,
    const char* reason) noexcept {
    state.sticky_failure.store(true, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> guard(state.failure_mu);
        if (state.failure_reason.empty()) state.failure_reason.assign(reason);
    } catch (...) {
    }
}

[[nodiscard]] std::string metal_archive_failure_reason(
    metal_pipeline_archive_interpose& state) {
    std::lock_guard<std::mutex> guard(state.failure_mu);
    return state.failure_reason.empty()
        ? std::string{"immutable Metal archive receipt failed"}
        : state.failure_reason;
}

struct metal_archive_file_snapshot {
    runtime_metal_archive_spec actual{};
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
        const metal_archive_file_snapshot&) const noexcept = default;
};

[[nodiscard]] std::expected<metal_archive_file_snapshot, std::string>
capture_metal_archive_file_snapshot(
    const std::filesystem::path& physical_path,
    const runtime_metal_archive_spec& expected,
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
    sealed_shader_cache_detail::sha256_hasher hasher;
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
    runtime_metal_archive_spec actual{
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
    return metal_archive_file_snapshot{
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

class private_metal_archive_snapshot {
public:
    private_metal_archive_snapshot() = default;
    explicit private_metal_archive_snapshot(std::filesystem::path path)
        : path_(std::move(path)) {}
    private_metal_archive_snapshot(const private_metal_archive_snapshot&) = delete;
    private_metal_archive_snapshot& operator=(
        const private_metal_archive_snapshot&) = delete;
    private_metal_archive_snapshot(
        private_metal_archive_snapshot&& other) noexcept
        : path_(std::exchange(other.path_, {})) {}
    private_metal_archive_snapshot& operator=(
        private_metal_archive_snapshot&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }
    ~private_metal_archive_snapshot() { cleanup(); }

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
static_assert(std::is_nothrow_move_constructible_v<private_metal_archive_snapshot>);

class private_snapshot_file_descriptor {
public:
    explicit private_snapshot_file_descriptor(int fd) noexcept : fd_(fd) {}
    private_snapshot_file_descriptor(const private_snapshot_file_descriptor&) = delete;
    private_snapshot_file_descriptor& operator=(
        const private_snapshot_file_descriptor&) = delete;
    ~private_snapshot_file_descriptor() noexcept {
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

class private_snapshot_unlink_guard {
public:
    explicit private_snapshot_unlink_guard(const char* path) noexcept
        : path_(path) {}
    private_snapshot_unlink_guard(const private_snapshot_unlink_guard&) = delete;
    private_snapshot_unlink_guard& operator=(
        const private_snapshot_unlink_guard&) = delete;
    ~private_snapshot_unlink_guard() noexcept {
        if (path_ != nullptr) (void)::unlink(path_);
    }

    void release() noexcept { path_ = nullptr; }

private:
    const char* path_ = nullptr;
};

[[nodiscard]] std::expected<private_metal_archive_snapshot, std::string>
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
    private_snapshot_file_descriptor fd_owner{fd};
    private_snapshot_unlink_guard unlink_owner{writable.data()};
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
    private_metal_archive_snapshot result{std::move(snapshot_path)};
    unlink_owner.release();
    return std::expected<private_metal_archive_snapshot, std::string>{
        std::in_place, std::move(result)};
}

class private_metal_archive_process_directory {
public:
    private_metal_archive_process_directory() = default;
    explicit private_metal_archive_process_directory(std::filesystem::path path)
        : path_(std::move(path)) {}
    private_metal_archive_process_directory(
        const private_metal_archive_process_directory&) = delete;
    private_metal_archive_process_directory& operator=(
        const private_metal_archive_process_directory&) = delete;
    private_metal_archive_process_directory(
        private_metal_archive_process_directory&& other) noexcept
        : path_(std::exchange(other.path_, {})) {}
    ~private_metal_archive_process_directory() noexcept {
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

[[nodiscard]] std::expected<private_metal_archive_process_directory, std::string>
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
    private_metal_archive_process_directory owner{
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

class open_private_metal_archive {
public:
    open_private_metal_archive() = default;
    open_private_metal_archive(
        int fd,
        std::filesystem::path path,
        sealed_shader_cache_detail::regular_file_identity identity) noexcept
        : fd_(fd), path_(std::move(path)), identity_(identity) {}
    open_private_metal_archive(const open_private_metal_archive&) = delete;
    open_private_metal_archive& operator=(const open_private_metal_archive&) = delete;
    open_private_metal_archive(open_private_metal_archive&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          path_(std::exchange(other.path_, {})),
          identity_(other.identity_),
          linked_(std::exchange(other.linked_, false)) {}
    ~open_private_metal_archive() noexcept {
        if (linked_ && !path_.empty()) (void)::unlink(path_.c_str());
        if (fd_ >= 0) (void)::close(fd_);
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] const sealed_shader_cache_detail::regular_file_identity&
    identity() const noexcept { return identity_; }

    [[nodiscard]] bool do_path_and_fd_identities_match() const noexcept {
        if (fd_ < 0 || path_.empty() || !linked_) return false;
        struct stat fd_status {};
        struct stat path_status {};
        return ::fstat(fd_, &fd_status) == 0
            && ::lstat(path_.c_str(), &path_status) == 0
            && S_ISREG(fd_status.st_mode) && S_ISREG(path_status.st_mode)
            && !S_ISLNK(path_status.st_mode)
            && sealed_shader_cache_detail::regular_file_identity_from_stat(
                   fd_status)
                == identity_
            && sealed_shader_cache_detail::regular_file_identity_from_stat(
                   path_status)
                == identity_;
    }

    [[nodiscard]] std::expected<void, std::string>
    unlink_close_and_require_absent()
    {
        if (!do_path_and_fd_identities_match()) {
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
            || sealed_shader_cache_detail::regular_file_identity_from_stat(
                   fd_status)
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
    sealed_shader_cache_detail::regular_file_identity identity_{};
    bool linked_ = true;
};

[[nodiscard]] std::expected<open_private_metal_archive, std::string>
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
    private_snapshot_file_descriptor fd_owner{fd};
    private_snapshot_unlink_guard unlink_owner{path.c_str()};
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
    open_private_metal_archive result{
        fd_owner.get(), path,
        sealed_shader_cache_detail::regular_file_identity_from_stat(status)};
    (void)fd_owner.release();
    unlink_owner.release();
    return result;
}

[[nodiscard]] std::expected<std::uint64_t, std::string>
hash_open_private_metal_archive_once(
    open_private_metal_archive& image,
    const runtime_metal_archive_spec& expected,
    std::span<const std::byte> exact_seed_bytes)
{
    if (!image.do_path_and_fd_identities_match()
        || exact_seed_bytes.size() != expected.size
        || ::lseek(image.fd(), 0, SEEK_SET) < 0) {
        return std::unexpected(
            "private Metal archive is not stable before qualification hash");
    }
    sealed_shader_cache_detail::sha256_hasher hasher;
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
        || !image.do_path_and_fd_identities_match()) {
        return std::unexpected(
            "private Metal archive qualification hash mismatch");
    }
    return streamed;
}

[[nodiscard]] void* load_metal_binary_archive(
    metal_pipeline_archive_interpose& state,
    void* loader_device,
    const std::filesystem::path& path)
{
    void* desc_cls = state.objc.get_class("MTLBinaryArchiveDescriptor");
    if (desc_cls == nullptr || loader_device == nullptr) {
        return nullptr;
    }
    using new_function = void* (*)(void*, void*);
    void* desc = objc_msg_cast<new_function>(state.objc.msg_send)(
        desc_cls,
        state.objc.sel_register_name("new"));
    scoped_objc_release desc_owner{
        &state.objc, state.sel_release, desc};
    void* url = bioseq_make_file_url(state.objc, path);
    if (desc == nullptr || url == nullptr) {
        return nullptr;
    }
    using set_object_function = void (*)(void*, void*, void*);
    objc_msg_cast<set_object_function>(state.objc.msg_send)(
        desc,
        state.objc.sel_register_name("setUrl:"),
        url);
    void* error = nullptr;
    using new_archive_function = void* (*)(void*, void*, void*, void**);
    void* archive = objc_msg_cast<new_archive_function>(state.objc.msg_send)(
        loader_device,
        state.objc.sel_register_name("newBinaryArchiveWithDescriptor:error:"),
        desc,
        &error);
    if (archive == nullptr) {
        bioseq_log<logging_level::debug>(
            "[vulkan_runtime] failed to load Metal binary archive {}: {}",
            path.string(),
            bioseq_ns_error_description(state.objc, error));
    }
    return archive;
}

[[nodiscard]] bool load_exact_metal_binary_archives(
    metal_pipeline_archive_interpose& state,
    void* loader_device) {
    state.loaded_pre_post_identity_checks = 0u;
    if (!state.configuration_image
        || !state.configuration_image->is_valid()) {
        mark_metal_archive_sticky_failure(
            state, "immutable Metal archive configuration image is absent");
        return false;
    }
    const runtime_metal_archive_configuration& configuration =
        state.configuration_image->configuration();
    const runtime_metal_archive_set_identity expected_identity =
        make_runtime_metal_archive_set_identity(configuration.archives);
    const std::uint64_t residency_metadata =
        bioseq::common::saturating_add(
            bioseq::common::saturating_mul(
                expected_identity.archive_count,
                sizeof(runtime_metal_archive_spec) + 2u * sizeof(void*)),
            2u * 1024u);
    const std::uint64_t residency_peak =
        bioseq::common::saturating_mul(
            bioseq::common::saturating_add(
                expected_identity.total_size, residency_metadata),
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (!expected_identity.is_valid()
        || residency_peak == (std::numeric_limits<std::uint64_t>::max)()) {
        mark_metal_archive_sticky_failure(
            state, "immutable Metal archive residency admission saturated");
        return false;
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto residency_permit =
        bioseq::parallelism::memory_admission_controller::instance()
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
    using allocate_function = void* (*)(void*, void*);
    using initialize_capacity_function = void* (*)(void*, void*, unsigned long);
    void* allocated = objc_msg_cast<allocate_function>(state.objc.msg_send)(
        array_cls,
        state.objc.sel_register_name("alloc"));
    void* array = objc_msg_cast<initialize_capacity_function>(state.objc.msg_send)(
        allocated,
        state.objc.sel_register_name("initWithCapacity:"),
        static_cast<unsigned long>(configuration.archives.size()));
    scoped_objc_release array_owner{
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
    using add_object_function = void (*)(void*, void*, void*);
    std::uint64_t hashed_bytes = 0u;
    std::uint64_t private_files_created = 0u;
    std::uint64_t private_files_unlinked = 0u;
    for (std::size_t archive_index = 0u;
         archive_index < configuration.archives.size(); ++archive_index) {
        const runtime_metal_archive_spec& expected =
            configuration.archives[archive_index];
        const sealed_shader_cache_artifact_image* seed_artifact =
            configuration.seed_image->artifact(
                expected.artifact_ordinal);
        if (seed_artifact == nullptr
            || seed_artifact->descriptor.kind
                != sealed_shader_cache_artifact_kind::metal_binary_archive
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
        if (!private_image->do_path_and_fd_identities_match()) {
            mark_metal_archive_sticky_failure(
                state,
                "private Metal archive path/descriptor identity changed before synchronous load");
            return false;
        }
        if (void* archive = load_metal_binary_archive(
                state, loader_device, private_image->path());
            archive != nullptr) {
            scoped_objc_release archive_owner{
                &state.objc, state.sel_release, archive};
            if (!private_image->do_path_and_fd_identities_match()) {
                mark_metal_archive_sticky_failure(
                    state,
                    fmt::format(
                        "private Metal archive identity changed across synchronous load: {}",
                        expected.artifact_filename));
                return false;
            }
            objc_msg_cast<add_object_function>(state.objc.msg_send)(
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
    using copy_function = void* (*)(void*, void*);
    void* immutable_array = objc_msg_cast<copy_function>(state.objc.msg_send)(
        array, state.objc.sel_register_name("copy"));
    scoped_objc_release immutable_array_owner{
        &state.objc, state.sel_release, immutable_array};
    using count_function = unsigned long (*)(void*, void*);
    const std::uint64_t immutable_count = immutable_array == nullptr
        ? 0u
        : static_cast<std::uint64_t>(
            objc_msg_cast<count_function>(state.objc.msg_send)(
                immutable_array, state.sel_count));
    if (immutable_array == nullptr
        || immutable_count != expected_identity.archive_count) {
        mark_metal_archive_sticky_failure(
            state, "cannot retain immutable Metal archive NSArray");
        return false;
    }
    const sealed_shader_cache_manifest& manifest =
        configuration.seed_image->manifest();
    const sealed_shader_cache_seed_receipt& seed_receipt =
        configuration.seed_image->receipt();
    runtime_metal_archive_process_qualification qualification{
        .wire_revision = 2u,
        .archive_set = expected_identity,
        .receipt_count = configuration.qualification_receipt_count,
        .hash_passes = 1u,
        .hashed_bytes = hashed_bytes,
        .private_files_created = private_files_created,
        .private_files_unlinked = private_files_unlinked,
        .live_private_paths = 0u,
        .pipeline_filesystem_hash_passes = 0u,
        .pipeline_filesystem_hashed_bytes = 0u,
        .helper_wire_revision = configuration.helper_wire_revision,
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
    if (!qualification.is_internally_consistent()) {
        mark_metal_archive_sticky_failure(
            state, "process Metal archive qualification is incomplete");
        return false;
    }
    state.archive_residency_peak_bytes = residency_peak;
    state.archive_residency_retained_bytes =
        bioseq::common::saturating_add(
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
    bioseq_log<logging_level::debug>(
        "[vulkan_runtime] loaded exact sealed Metal archive set count={} bytes={} checksum={}",
        state.loaded_identity.archive_count,
        state.loaded_identity.total_size,
        state.loaded_identity.ordered_checksum);
    return true;
}

[[nodiscard]] std::expected<std::uint64_t, std::string>
verify_resident_metal_archive_snapshots(
    metal_pipeline_archive_interpose& state,
    std::string_view operation_label) {
    if (!state.configuration_image || state.archive_array == nullptr
        || !state.process_qualification.is_internally_consistent()) {
        return std::unexpected(
            fmt::format(
                "{} process Metal archive qualification is absent", operation_label));
    }
    if (state.loaded_identity != state.process_qualification.archive_set
        || state.process_qualification.live_private_paths != 0u
        || state.process_qualification.pipeline_filesystem_hash_passes != 0u
        || state.process_qualification.pipeline_filesystem_hashed_bytes != 0u) {
        return std::unexpected(fmt::format(
            "{} process Metal archive qualification identity mismatch",
            operation_label));
    }
    return 0u;
}

[[nodiscard]] std::string descriptor_function_name(metal_pipeline_archive_interpose& state, void* desc)
{
    if (desc == nullptr || state.sel_compute_function == nullptr || state.sel_name == nullptr) {
        return {};
    }
    using get_object_function = void* (*)(void*, void*);
    void* function = objc_msg_cast<get_object_function>(state.objc.msg_send)(
        desc,
        state.sel_compute_function);
    void* name = function == nullptr
        ? nullptr
        : objc_msg_cast<get_object_function>(state.objc.msg_send)(function, state.sel_name);
    return bioseq_ns_string_utf8(state.objc, name, state.sel_utf8_string);
}

struct metal_error_identity {
    std::string domain;
    std::int64_t code = 0;
};

[[nodiscard]] metal_error_identity metal_error_identity_from_error(
    metal_pipeline_archive_interpose& state,
    void* error) {
    if (error == nullptr || state.sel_error_domain == nullptr
        || state.sel_error_code == nullptr) {
        return {};
    }
    using get_object_function = void* (*)(void*, void*);
    using code_function = std::intptr_t (*)(void*, void*);
    void* domain = objc_msg_cast<get_object_function>(state.objc.msg_send)(
        error, state.sel_error_domain);
    return metal_error_identity{
        .domain = bioseq_ns_string_utf8(
            state.objc, domain, state.sel_utf8_string),
        .code = static_cast<std::int64_t>(
            objc_msg_cast<code_function>(state.objc.msg_send)(
                error, state.sel_error_code)),
    };
}

constexpr unsigned long k_metal_pipeline_fail_on_binary_archive_miss = 1ul << 2u;

struct active_metal_archive_pipeline_scope {
    std::mutex mu;
    runtime_metal_archive_pipeline_audit audit{};
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
    metal_pipeline_archive_interpose& state,
    void* device) noexcept {
    if (device == nullptr || state.sel_registry_id == nullptr) return 0u;
    using registry_function = unsigned long long (*)(void*, void*);
    return static_cast<std::uint64_t>(
        objc_msg_cast<registry_function>(state.objc.msg_send)(
            device, state.sel_registry_id));
}

void release_objc_object(
    metal_pipeline_archive_interpose& state,
    void* object) noexcept {
    if (object == nullptr || state.sel_release == nullptr) return;
    using release_function = void (*)(void*, void*);
    objc_msg_cast<release_function>(state.objc.msg_send)(object, state.sel_release);
}

[[nodiscard]] void* make_empty_metal_archive_array(
    metal_pipeline_archive_interpose& state,
    void* device) noexcept {
    void* desc_cls = state.objc.get_class("MTLBinaryArchiveDescriptor");
    void* array_cls = state.objc.get_class("NSMutableArray");
    if (desc_cls == nullptr || array_cls == nullptr || device == nullptr) {
        return nullptr;
    }
    using new_function = void* (*)(void*, void*);
    void* desc = objc_msg_cast<new_function>(state.objc.msg_send)(
        desc_cls, state.objc.sel_register_name("new"));
    if (desc == nullptr) return nullptr;
    void* error = nullptr;
    using new_archive_function = void* (*)(void*, void*, void*, void**);
    void* archive = objc_msg_cast<new_archive_function>(state.objc.msg_send)(
        device,
        state.objc.sel_register_name("newBinaryArchiveWithDescriptor:error:"),
        desc,
        &error);
    release_objc_object(state, desc);
    if (archive == nullptr) return nullptr;

    using allocate_function = void* (*)(void*, void*);
    using initialize_capacity_function = void* (*)(void*, void*, unsigned long);
    void* allocated = objc_msg_cast<allocate_function>(state.objc.msg_send)(
        array_cls, state.objc.sel_register_name("alloc"));
    void* array = objc_msg_cast<initialize_capacity_function>(state.objc.msg_send)(
        allocated,
        state.objc.sel_register_name("initWithCapacity:"),
        1ul);
    if (array == nullptr) {
        release_objc_object(state, archive);
        return nullptr;
    }
    using add_object_function = void (*)(void*, void*, void*);
    objc_msg_cast<add_object_function>(state.objc.msg_send)(
        array, state.objc.sel_register_name("addObject:"), archive);
    release_objc_object(state, archive);
    return array;
}

void fail_active_metal_archive_scope(
    metal_pipeline_archive_interpose& state,
    active_metal_archive_pipeline_scope& scope,
    std::string reason) noexcept {
    scope.audit.sticky_failure = true;
    try {
        if (scope.failure.empty()) scope.failure = reason;
    } catch (...) {
    }
    mark_metal_archive_sticky_failure(state, std::move(reason));
}

enum class metal_pipeline_intercept_verdict : std::uint8_t {
    outside_scope,
    enforced,
    rejected,
};

[[nodiscard]] metal_pipeline_intercept_verdict prepare_scoped_metal_pipeline_call(
    void* self,
    void* desc,
    unsigned long options,
    bool asynchronous) noexcept {
    auto& state = metal_pipeline_archive_interpose_state();
    try {
    std::unique_lock<std::mutex> active_guard(state.active_scope_mu);
    const std::shared_ptr<active_metal_archive_pipeline_scope> scope =
        state.active_scope;
    if (scope == nullptr) {
        state.outside_scope_interceptions.fetch_add(
            1u, std::memory_order_relaxed);
        active_guard.unlock();
        if (state.installed) {
            mark_metal_archive_sticky_failure(
                state,
                "immutable Metal runtime intercepted an unscoped pipeline creation");
            return metal_pipeline_intercept_verdict::rejected;
        }
        return metal_pipeline_intercept_verdict::outside_scope;
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
            return metal_pipeline_intercept_verdict::rejected;
        }
        return metal_pipeline_intercept_verdict::outside_scope;
    }
    try {
        ++scope->audit.scoped_interceptions;
        if (scope->audit.scoped_interceptions != 1u) {
            fail_active_metal_archive_scope(
                state, *scope,
                "one Vulkan createInfo unexpectedly produced more than one Metal PSO interception");
            return metal_pipeline_intercept_verdict::rejected;
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
            return metal_pipeline_intercept_verdict::rejected;
        }
        scope->audit.observed_entry_name = observed_entry;
        ++scope->audit.matched_descriptor_functions;
        if (state.sticky_failure.load(std::memory_order_acquire)) {
            fail_active_metal_archive_scope(
                state, *scope, metal_archive_failure_reason(state));
            return metal_pipeline_intercept_verdict::rejected;
        }
        if (self == nullptr || desc == nullptr
            || state.archive_array == nullptr
            || state.original_new_pipeline_sync == nullptr) {
            fail_active_metal_archive_scope(
                state, *scope,
                "Metal archive interpose lacks descriptor, archive set, or original sync IMP");
            return metal_pipeline_intercept_verdict::rejected;
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
            return metal_pipeline_intercept_verdict::rejected;
        }
        if (scope->audit.intercepted_metal_registry_id != 0u
            && scope->audit.intercepted_metal_registry_id
                != intercepted_registry) {
            fail_active_metal_archive_scope(
                state, *scope,
                "one pipeline scope intercepted more than one Metal device");
            return metal_pipeline_intercept_verdict::rejected;
        }
        scope->audit.intercepted_metal_registry_id = intercepted_registry;
        auto& command_state = moltenvk_command_interpose_state();
        const command_interpose_device_binding command_binding =
            capture_command_interpose_device_binding(self);
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
            return metal_pipeline_intercept_verdict::rejected;
        }

        using count_function = unsigned long (*)(void*, void*);
        const unsigned long exact_count =
            objc_msg_cast<count_function>(state.objc.msg_send)(
                state.archive_array, state.sel_count);
        const std::size_t configured_archive_count =
            state.configuration_image
                ? state.configuration_image->configuration().archives.size()
                : 0u;
        if (exact_count != configured_archive_count) {
            fail_active_metal_archive_scope(
                state, *scope,
                "resident Metal archive object count differs from sealed set");
            return metal_pipeline_intercept_verdict::rejected;
        }

        void* empty_array = make_empty_metal_archive_array(state, self);
        if (empty_array == nullptr) {
            fail_active_metal_archive_scope(
                state, *scope,
                "cannot construct same-device empty archive negative control");
            return metal_pipeline_intercept_verdict::rejected;
        }
        using set_object_function = void (*)(void*, void*, void*);
        objc_msg_cast<set_object_function>(state.objc.msg_send)(
            desc, state.sel_set_binary_archives, empty_array);
        ++scope->audit.same_descriptor_negative_controls;

        void* negative_reflection = nullptr;
        void* negative_error = nullptr;
        using sync_function = void* (*)(
            void*, void*, void*, unsigned long, void**, void**);
        ++scope->audit.original_sync_negative_calls;
        void* unexpected_pipeline = objc_msg_cast<sync_function>(
            state.original_new_pipeline_sync)(
                self,
                state.sel_new_pipeline_sync,
                desc,
                options | k_metal_pipeline_fail_on_binary_archive_miss,
                &negative_reflection,
                &negative_error);
        if (unexpected_pipeline != nullptr || negative_error == nullptr) {
            release_objc_object(state, unexpected_pipeline);
            release_objc_object(state, empty_array);
            fail_active_metal_archive_scope(
                state, *scope,
                "empty archive negative control did not produce a binary-archive miss");
            return metal_pipeline_intercept_verdict::rejected;
        }
        const metal_error_identity negative_identity =
            metal_error_identity_from_error(state, negative_error);
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
            return metal_pipeline_intercept_verdict::rejected;
        }
        ++scope->audit.negative_control_misses;

        // This replacement is unconditional: a descriptor-provided array can
        // neither augment nor override the exact manifest-owned set.
        objc_msg_cast<set_object_function>(state.objc.msg_send)(
            desc, state.sel_set_binary_archives, state.archive_array);
        release_objc_object(state, empty_array);
        if (!state.process_qualification.is_internally_consistent()
            || state.process_qualification
                != scope->audit.cache_qualification) {
            fail_active_metal_archive_scope(
                state, *scope,
                "process Metal archive qualification changed before pipeline");
            return metal_pipeline_intercept_verdict::rejected;
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
        return metal_pipeline_intercept_verdict::enforced;
    } catch (const std::exception& error) {
        fail_active_metal_archive_scope(state, *scope, error.what());
        return metal_pipeline_intercept_verdict::rejected;
    } catch (...) {
        fail_active_metal_archive_scope(
            state, *scope, "unknown immutable Metal archive interpose failure");
        return metal_pipeline_intercept_verdict::rejected;
    }
    } catch (...) {
        mark_metal_archive_sticky_failure_literal(
            state,
            "immutable Metal archive interpose failed before scoped handling");
        return metal_pipeline_intercept_verdict::rejected;
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
    using async_function = void (*)(void*, void*, void*, unsigned long, void*);
    const metal_pipeline_intercept_verdict verdict =
        prepare_scoped_metal_pipeline_call(
            self, desc, options, true);
    if (verdict == metal_pipeline_intercept_verdict::outside_scope) {
        objc_msg_cast<async_function>(state.original_new_pipeline_async)(
            self, cmd, desc, options, completion_handler);
        return;
    }
    if (verdict == metal_pipeline_intercept_verdict::rejected) {
        invoke_metal_pipeline_completion(
            completion_handler, nullptr, nullptr, nullptr);
        return;
    }
    objc_msg_cast<async_function>(state.original_new_pipeline_async)(
        self,
        cmd,
        desc,
        options | k_metal_pipeline_fail_on_binary_archive_miss,
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
    using sync_function = void* (*)(void*, void*, void*, unsigned long, void**, void**);
    const metal_pipeline_intercept_verdict verdict =
        prepare_scoped_metal_pipeline_call(
            self, desc, options, false);
    if (verdict == metal_pipeline_intercept_verdict::outside_scope) {
        return objc_msg_cast<sync_function>(state.original_new_pipeline_sync)(
            self, cmd, desc, options, reflection, error);
    }
    if (verdict == metal_pipeline_intercept_verdict::rejected) {
        return nullptr;
    }
    return objc_msg_cast<sync_function>(state.original_new_pipeline_sync)(
        self,
        cmd,
        desc,
        options | k_metal_pipeline_fail_on_binary_archive_miss,
        reflection,
        error);
}

void install_metal_pipeline_archive_interpose(
    runtime_vulkan_device_identity selected_vulkan_device) noexcept {
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
    if (!configuration->is_valid()) {
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

    objc_runtime_functions objc{};
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

    using create_device_function = void* (*)();
    auto create_device = reinterpret_cast<create_device_function>(
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
    scoped_objc_release device_owner{
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
    const runtime_metal_archive_configuration& installed_configuration =
        state.configuration_image->configuration();
    state.configured_identity = make_runtime_metal_archive_set_identity(
        installed_configuration.archives);
    state.configuration_validated = state.configured_identity.is_valid();
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
    if (!selected_vulkan_device.is_valid() || selected_uuid_missing
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
        bioseq_log<logging_level::debug>(
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

class metal_archive_pipeline_create_scope {
public:
    metal_archive_pipeline_create_scope(
        bool required,
        std::array<std::uint8_t, VK_UUID_SIZE> runtime_device_uuid,
        shader_variant_key shader_key,
        std::string expected_entry_name)
        : required_(required) {
        auto& state = metal_pipeline_archive_interpose_state();
        if (!is_runtime_metal_archive_configuration_installed()
            || !state.configuration_image) {
            if (required_) {
                failure_ =
                    "provider-backed Vulkan pipeline has no sealed Metal archive configuration";
            }
            return;
        }
        const runtime_metal_archive_configuration& configuration =
            state.configuration_image->configuration();
        const sealed_shader_cache_manifest& manifest =
            configuration.seed_image->manifest();
        const sealed_shader_cache_seed_receipt& seed_receipt =
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
            make_runtime_metal_archive_set_identity(configuration.archives);
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
            || !audit_.cache_qualification.is_internally_consistent()) {
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
        active_ = std::make_shared<active_metal_archive_pipeline_scope>();
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

    metal_archive_pipeline_create_scope(
        const metal_archive_pipeline_create_scope&) = delete;
    metal_archive_pipeline_create_scope& operator=(
        const metal_archive_pipeline_create_scope&) = delete;

    ~metal_archive_pipeline_create_scope() noexcept {
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

    [[nodiscard]] bool is_ready_for_pipeline_call() const noexcept {
        return failure_.empty() && (active_scope_ || !required_);
    }

    [[nodiscard]] std::expected<
        std::optional<runtime_metal_archive_pipeline_audit>, std::string>
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
            return std::optional<runtime_metal_archive_pipeline_audit>{};
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
        if (!state.process_qualification.is_internally_consistent()
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
        if (!audit_.is_internally_consistent()) {
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
        return std::optional<runtime_metal_archive_pipeline_audit>{
            std::move(audit_)};
    }

private:
    bool required_ = false;
    bool active_scope_ = false;
    bool finished_ = false;
    std::string failure_;
    runtime_metal_archive_pipeline_audit audit_{};
    std::shared_ptr<active_metal_archive_pipeline_scope> active_{};
};
#else
void install_moltenvk_command_library_interpose() noexcept {}
void install_metal_pipeline_archive_interpose(
    runtime_vulkan_device_identity) noexcept {}

class metal_archive_pipeline_create_scope {
public:
    metal_archive_pipeline_create_scope(
        bool required,
        std::array<std::uint8_t, VK_UUID_SIZE>,
        shader_variant_key,
        std::string) noexcept
        : required_(required || is_runtime_metal_archive_configuration_installed()) {}

    [[nodiscard]] bool is_ready_for_pipeline_call() const noexcept {
        return !required_;
    }

    [[nodiscard]] std::expected<
        std::optional<runtime_metal_archive_pipeline_audit>, std::string>
    finish(VkResult) const {
        if (required_) {
            return std::unexpected(
                "immutable Metal archive receipt requested on a non-Apple runtime");
        }
        return std::optional<runtime_metal_archive_pipeline_audit>{};
    }

private:
    bool required_ = false;
};
#endif

void require_immutable_metal_policy_live(std::string_view operation_label) {
    runtime_metal_archive_configuration_slot_instance()
        .live_gate_calls.fetch_add(1u, std::memory_order_relaxed);
#if defined(__APPLE__)
    if (!is_runtime_metal_archive_configuration_installed()) return;
    auto& archive_state = metal_pipeline_archive_interpose_state();
    auto& command_state = moltenvk_command_interpose_state();
    const bool clean = archive_state.configuration_validated
        && archive_state.configured_identity.is_valid()
        && archive_state.configured_identity == archive_state.loaded_identity
        && archive_state.installed
        && archive_state.archive_array != nullptr
        && archive_state.process_qualification.is_internally_consistent()
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
        throw vulkan_runtime_error(fmt::format(
            "{} observed stale or poisoned immutable Metal policy state",
            operation_label));
    }
#else
    (void)operation_label;
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
        throw vulkan_runtime_error(fmt::format(
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
        throw vulkan_runtime_error(fmt::format(
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

[[nodiscard]] std::expected<runtime_private_archive_snapshot_test_result, std::string>
verify_runtime_private_archive_snapshot_for_test(
    std::string_view artifact_filename,
    std::span<const std::byte> bytes,
    const std::filesystem::path& temp_parent)
{
#if defined(__APPLE__)
    if (artifact_filename.empty() || bytes.empty()) {
        return std::unexpected("private archive snapshot test input is empty");
    }
    runtime_metal_archive_spec expected{
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
    return runtime_private_archive_snapshot_test_result{
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
// buffer cleanup
// ---------------------------------------------------------------------------

void buffer::reset() noexcept {
    if (owner != nullptr && buffer_handle != VK_NULL_HANDLE) {
        owner->destroy_buffer(*this);
    }
    owner = nullptr;
    buffer_handle = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    bytes = 0;
    allocation_bytes = 0;
    memory_properties = 0;
    process_footprint_reflected = false;
    mapped = nullptr;
}

// ---------------------------------------------------------------------------
// runtime lifecycle
// ---------------------------------------------------------------------------

runtime::runtime() {
    try {
        init();
    } catch (const std::exception& e) {
        init_error_ = e.what();
        bioseq_log<logging_level::debug>("[vulkan_runtime] init failed: {}", e.what());
        shutdown();
        features_ = vulkan_feature_set{};
    } catch (...) {
        init_error_ = "unknown Vulkan initialization failure";
        bioseq_log<logging_level::debug>("[vulkan_runtime] init failed (unknown)");
        shutdown();
        features_ = vulkan_feature_set{};
    }
}

runtime::~runtime() {
    shutdown();
}

runtime& runtime::instance() {
    static runtime runtime;
    return runtime;
}

void runtime::resolve_moltenvk_private_api() noexcept {
    set_workgroup_size_mvk_ = nullptr;
#if defined(__APPLE__)
    auto resolve_from = [](void* handle) noexcept
        -> set_workgroup_size_mvk_function {
        if (handle == nullptr) return nullptr;
        return reinterpret_cast<set_workgroup_size_mvk_function>(
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

bool runtime::should_use_shader_provider_locked() const noexcept {
    runtime_shader_provider* provider = runtime_shader_provider_slot();
    return provider != nullptr && provider->load_module != nullptr;
}

void prepare_shader_provider_runtime() {
    runtime_shader_provider* provider = runtime_shader_provider_slot();
    if (provider == nullptr || provider->prepare_runtime == nullptr) {
        return;
    }
    auto prepared = provider->prepare_runtime();
    if (!prepared) {
        throw vulkan_runtime_error(fmt::format(
            "runtime shader provider prepare failed: {}", prepared.error()));
    }
}

void runtime::init() {
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
    if (is_runtime_metal_archive_configuration_installed()) {
        auto& command_state = moltenvk_command_interpose_state();
        if (!command_state.installed
            || !command_state.immutable_fail_closed) {
            throw vulkan_runtime_error(
                "immutable GPU mode could not install fail-closed MoltenVK command-source interpose");
        }
    }
#endif
    const auto inst_exts = instance_extensions();
    std::vector<const char*> enabled_inst_exts;
    VkInstanceCreateFlags inst_flags = 0;
    if (has_extension(inst_exts, k_portability_enumeration)) {
        enabled_inst_exts.push_back(k_portability_enumeration);
        inst_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    if (has_extension(inst_exts, k_get_physical_device_properties2)) {
        enabled_inst_exts.push_back(k_get_physical_device_properties2);
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
    const bool has_layer_settings = has_extension(inst_exts, k_layer_settings);
    if (has_layer_settings) {
        enabled_inst_exts.push_back(k_layer_settings);
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
    if (is_runtime_metal_archive_configuration_installed()) {
        auto& command_state = moltenvk_command_interpose_state();
        if (command_state.sticky_failure.load(std::memory_order_acquire)
            || command_state.source_fallback_calls.load(
                std::memory_order_relaxed) != 0u) {
            throw vulkan_runtime_error(
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
        throw vulkan_runtime_error("no Vulkan physical devices");
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
            continue;  // Vulkan chaining requires native 64-bit ints in the shader
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
            has_extension(device_extensions(candidate), k_portability_subset);
        device_chosen = true;
        break;
    }
    if (!device_chosen) {
        throw vulkan_runtime_error("no Vulkan device with a compute queue + shaderInt64");
    }

    // --- Logical device (enable shaderInt64) ---
    std::vector<const char*> enabled_dev_exts;
    if (device_has_portability_subset) {
        enabled_dev_exts.push_back(k_portability_subset);
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
    runtime_vulkan_device_identity selected_device_identity{
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
        && is_runtime_metal_archive_configuration_installed()) {
        auto& archive_state = metal_pipeline_archive_interpose_state();
        mark_metal_archive_sticky_failure(
            archive_state,
            fmt::format(
                "vkCreateDevice failed after immutable Metal archive install ({})",
                static_cast<std::int32_t>(create_device_result)));
        rollback_metal_pipeline_archive_interpose_install(archive_state);
    }
    if (is_runtime_metal_archive_configuration_installed()) {
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
            throw vulkan_runtime_error(fmt::format(
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
    runtime_vulkan_device_identity verified_device_identity{
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
        throw vulkan_runtime_error(
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
        if (is_runtime_metal_archive_configuration_installed()) {
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
    if (has_extension(inst_exts, k_get_physical_device_properties2)) {
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
            device_local = std::max(device_local, mem_props.memoryHeaps[h].size);
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

    std::array<
        VkCommandBuffer,
        k_rank_prefilter_replay_route_count + 1u> command_buffers{};
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = command_pool_;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = static_cast<std::uint32_t>(
        command_buffers.size());
    check_vk(vkAllocateCommandBuffers(
                 device_, &cmd_alloc, command_buffers.data()),
             "vkAllocateCommandBuffers");
    command_buffer_ = command_buffers[0];
    for (std::size_t replay_index = 0u;
         replay_index < prefilter_recorded_commands_.size();
         ++replay_index) {
        prefilter_recorded_commands_[replay_index].command_buffer =
            command_buffers[replay_index + 1u];
    }

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
    push_range.size = sizeof(chaining_push_constants);

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

    // Pick the largest power-of-two anchor count the per-query
    // bitonic sort can hold in one workgroup: bounded by workgroup invocations
    // and by shared memory (each lane holds a 24-byte AnchorWire + a 4-byte
    // original index). Anchor spans above this capacity are routed to the
    // scalable stable GPU radix path; selected Vulkan lookup never host-sorts.
    {
        gpu_anchor_sort_capacity_ = compute_seed_anchor_bitonic_capacity(
            features_.max_workgroup_invocations,
            features_.max_workgroup_size_x,
            features_.max_shared_memory_bytes);
    }

    features_.available = true;
    std::strncpy(features_.device_name.data(), props.deviceName,
                 features_.device_name.size() - 1u);
    features_.device_name.back() = '\0';
    init_error_.clear();
    bioseq_log<logging_level::debug>(
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

void runtime::shutdown() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        prefilter_recorded_commands_ = {};
        release_prefilter_workspace_locked();
        // Release arena buffers (host-visible, owned via buffer RAII)
        // and the GPU frontend descriptor pool before the device goes away.
        gpu_arena_.reset();
        if (dispatch_descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, dispatch_descriptor_pool_, nullptr);
            dispatch_descriptor_pool_ = VK_NULL_HANDLE;
        }
        std::vector<pipeline_entry>{}.swap(pipeline_cache_);
        std::vector<owned_compute_pipeline>{}.swap(compute_pipelines_);
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
    // runtime initialization can still fail after vkCreateDevice.  Never
    // leave process-wide Metal IMPs or resident archive authority behind when
    // the owning Vulkan runtime is unavailable or being destroyed.
    rollback_metal_pipeline_archive_interpose_install(
        metal_pipeline_archive_interpose_state());
#endif
}

std::uint32_t runtime::find_memory_type(
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
    throw vulkan_runtime_error("no compatible Vulkan memory type");
}

// ---------------------------------------------------------------------------
// buffer allocation
// ---------------------------------------------------------------------------

buffer runtime::create_host_buffer(std::uint64_t bytes, VkBufferUsageFlags usage) {
    if (!features_.available) {
        throw vulkan_runtime_error("create_host_buffer on unavailable device");
    }
    if (bytes == 0) bytes = 1;  // zero-sized VkBuffers are invalid

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = bytes;
    buf_info.usage = usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer_handle = VK_NULL_HANDLE;
    check_vk(
        vkCreateBuffer(device_, &buf_info, nullptr, &buffer_handle),
        "vkCreateBuffer(host)");

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, buffer_handle, &reqs);

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
        vkDestroyBuffer(device_, buffer_handle, nullptr);
        throw vulkan_runtime_error("vkAllocateMemory(host)");
    }
    if (vkBindBufferMemory(device_, buffer_handle, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyBuffer(device_, buffer_handle, nullptr);
        throw vulkan_runtime_error("vkBindBufferMemory(host)");
    }
    void* mapped = nullptr;
    if (vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyBuffer(device_, buffer_handle, nullptr);
        throw vulkan_runtime_error("vkMapMemory(host)");
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
    return buffer{this, buffer_handle, memory, bytes, reqs.size,
                  selected_properties, footprint_reflected, mapped};
}

std::uint64_t runtime::buffer_memory_requirement_locked(
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

std::uint64_t
runtime::prefilter_workspace_allocation_bytes_locked() const noexcept
{
    using bioseq::common::saturating_add;
    return saturating_add(
        prefilter_query_buffer_.allocation_bytes,
        saturating_add(
            prefilter_target_buffer_.allocation_bytes,
            saturating_add(
                prefilter_jobs_buffer_.allocation_bytes,
                prefilter_results_buffer_.allocation_bytes)));
}

void runtime::release_prefilter_workspace_locked() noexcept
{
    prefilter_results_buffer_.reset();
    prefilter_jobs_buffer_.reset();
    prefilter_target_buffer_.reset();
    prefilter_query_buffer_.reset();
    prefilter_descriptor_binding_cache_ = {};
    prefilter_workspace_permit_.release();
}

void runtime::ensure_prefilter_workspace_buffer_locked(
    buffer& allocation,
    std::uint64_t bytes,
    VkBufferUsageFlags usage)
{
    if (allocation.is_valid() && allocation.bytes >= bytes) return;

    using bioseq::common::saturating_add;
    const std::uint64_t allocation_requirement =
        buffer_memory_requirement_locked(bytes, usage);
    const std::uint64_t peak = saturating_add(
        prefilter_workspace_allocation_bytes_locked(),
        allocation_requirement);
    if (peak == (std::numeric_limits<std::uint64_t>::max)()) {
        throw vulkan_runtime_error(
            "rank-prefilter workspace admission estimate saturated");
    }

    bioseq::parallelism::memory_admission_failure failure{};
    if (!prefilter_workspace_permit_) {
        auto permit = bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(peak, &failure);
        if (!permit) {
            throw vulkan_runtime_error(fmt::format(
                "rank-prefilter workspace admission denied: reason={} requested={}",
                bioseq::parallelism::memory_admission_failure_reason_name(
                    failure.reason),
                failure.requested_bytes));
        }
        prefilter_workspace_permit_ = std::move(*permit);
    } else if (!prefilter_workspace_permit_.grow_to(peak, &failure)) {
        throw vulkan_runtime_error(fmt::format(
            "rank-prefilter workspace growth denied: reason={} requested={}",
            bioseq::parallelism::memory_admission_failure_reason_name(
                failure.reason),
            failure.requested_bytes));
    }

    try {
        buffer replacement = create_device_buffer(bytes, usage);
        if (replacement.mapped == nullptr) {
            throw vulkan_runtime_error(
                "rank-prefilter workspace allocation is not host-mapped");
        }
        if (replacement.allocation_bytes != allocation_requirement) {
            throw vulkan_runtime_error(
                "rank-prefilter workspace allocation requirement changed");
        }
        allocation = std::move(replacement);
    } catch (...) {
        const std::uint64_t retained =
            prefilter_workspace_allocation_bytes_locked();
        if (retained <= prefilter_workspace_permit_.bytes()) {
            prefilter_workspace_permit_.shrink_to(retained);
            if (retained == 0u) prefilter_workspace_permit_.release();
        }
        throw;
    }

    const std::uint64_t retained =
        prefilter_workspace_allocation_bytes_locked();
    if (retained == (std::numeric_limits<std::uint64_t>::max)()
        || retained > prefilter_workspace_permit_.bytes()) {
        release_prefilter_workspace_locked();
        throw vulkan_runtime_error(
            "rank-prefilter workspace exceeded admitted allocation");
    }
    prefilter_workspace_permit_.shrink_to(retained);
}

std::uint64_t runtime::gpu_arena_allocation_requirement(
    std::uint64_t requested_bytes)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error(
            "gpu_arena_allocation_requirement on unavailable device");
    }
    const std::uint64_t requested_requirement =
        buffer_memory_requirement_locked(
            requested_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // The GPU frontend arena is persistent.  A smaller later case reuses the prior
    // high-water allocation, so its count admission must cover that actual live
    // allocation rather than only the new case's requested VkBuffer size.
    const std::uint64_t current_requirement = gpu_arena_
        ? gpu_arena_->device_buffer().allocation_bytes
        : 0u;
    return std::max(requested_requirement, current_requirement);
}

std::uint64_t runtime::buffer_allocation_requirement(
    std::uint64_t requested_bytes,
    VkBufferUsageFlags usage)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error(
            "buffer_allocation_requirement on unavailable device");
    }
    return buffer_memory_requirement_locked(requested_bytes, usage);
}

buffer runtime::create_device_buffer(std::uint64_t bytes, VkBufferUsageFlags usage) {
    if (!features_.available) {
        throw vulkan_runtime_error("create_device_buffer on unavailable device");
    }
    if (bytes == 0) bytes = 1;

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = bytes;
    buf_info.usage = usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer_handle = VK_NULL_HANDLE;
    check_vk(
        vkCreateBuffer(device_, &buf_info, nullptr, &buffer_handle),
        "vkCreateBuffer(device)");

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, buffer_handle, &reqs);

    // Vulkan chaining targets unified memory: prefer device-local that is ALSO
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
    } catch (const vulkan_runtime_error&) {
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
        vkDestroyBuffer(device_, buffer_handle, nullptr);
        throw vulkan_runtime_error("vkAllocateMemory(device)");
    }
    if (vkBindBufferMemory(device_, buffer_handle, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyBuffer(device_, buffer_handle, nullptr);
        throw vulkan_runtime_error("vkBindBufferMemory(device)");
    }
    // Map persistently — on unified memory the device-local buffer is
    // host-visible, so the round-trip writes inputs / reads outputs directly.
    void* mapped = nullptr;
    if (vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        // Not host-visible: leave unmapped. The round-trip rejects this case
        // (Vulkan chaining requires unified memory) rather than silently mis-staging.
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
    return buffer{this, buffer_handle, memory, bytes, reqs.size,
                  selected_properties, footprint_reflected, mapped};
}

void runtime::destroy_buffer(buffer& allocation) noexcept {
    if (device_ == VK_NULL_HANDLE) return;
    if (allocation.mapped != nullptr && allocation.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, allocation.memory);
    }
    if (allocation.buffer_handle != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, allocation.buffer_handle, nullptr);
    }
    if (allocation.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, allocation.memory, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Pipeline creation / cache
// ---------------------------------------------------------------------------

std::uint64_t runtime::pipeline_cache_metadata_bytes_locked() const noexcept {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    std::uint64_t owned = saturating_mul(
        static_cast<std::uint64_t>(pipeline_cache_.capacity()),
        sizeof(pipeline_entry));
    owned = saturating_add(
        owned,
        saturating_mul(
            static_cast<std::uint64_t>(compute_pipelines_.capacity()),
            sizeof(owned_compute_pipeline)));
    for (const pipeline_entry& entry : pipeline_cache_) {
        owned = saturating_add(owned, entry.entry_name.capacity());
        if (entry.metal_archive_receipt) {
            owned = saturating_add(
                owned,
                entry.metal_archive_receipt->owned_capacity_bytes());
        }
    }
    for (const owned_compute_pipeline& entry : compute_pipelines_) {
        owned = saturating_add(owned, entry.entry_name.capacity());
        if (entry.metal_archive_receipt) {
            owned = saturating_add(
                owned,
                entry.metal_archive_receipt->owned_capacity_bytes());
        }
    }
    return owned;
}

void runtime::admit_pipeline_cache_growth_locked(
    std::size_t normal_capacity,
    std::size_t compute_pipeline_capacity) {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    std::uint64_t projected = pipeline_cache_metadata_bytes_locked();
    if (normal_capacity > pipeline_cache_.capacity()) {
        projected = saturating_add(
            projected,
            saturating_mul(
                static_cast<std::uint64_t>(
                    normal_capacity - pipeline_cache_.capacity()),
                sizeof(pipeline_entry)));
    }
    if (compute_pipeline_capacity > compute_pipelines_.capacity()) {
        projected = saturating_add(
            projected,
            saturating_mul(
                static_cast<std::uint64_t>(
                    compute_pipeline_capacity - compute_pipelines_.capacity()),
                sizeof(owned_compute_pipeline)));
    }
    // Covers one pending entry, receipt allocation/control metadata, all five
    // bounded audit strings, and vector/string allocator headroom.
    projected = saturating_add(
        projected,
        sizeof(runtime_metal_archive_pipeline_receipt)
            + sizeof(runtime_metal_archive_pipeline_audit) + 16u * 1024u);
    const std::uint64_t peak = saturating_mul(
        projected, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (peak == (std::numeric_limits<std::uint64_t>::max)()) {
        throw vulkan_runtime_error(
            "pipeline cache metadata admission estimate saturated");
    }
    bioseq::parallelism::memory_admission_failure failure{};
    if (!pipeline_cache_metadata_permit_) {
        auto permit = bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(peak, &failure);
        if (!permit) {
            throw vulkan_runtime_error(shader_cache_message(
                "pipeline cache metadata admission denied before Vulkan creation: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(
                    failure.reason),
                " requested=", failure.requested_bytes));
        }
        pipeline_cache_metadata_permit_ = std::move(*permit);
    } else if (!pipeline_cache_metadata_permit_.grow_to(peak, &failure)) {
        throw vulkan_runtime_error(shader_cache_message(
            "pipeline cache metadata growth denied before Vulkan creation: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(
                failure.reason),
            " requested=", failure.requested_bytes));
    }
    try {
        pipeline_cache_.reserve(normal_capacity);
        compute_pipelines_.reserve(compute_pipeline_capacity);
    } catch (...) {
        reconcile_pipeline_cache_metadata_locked();
        throw;
    }
}

void runtime::reconcile_pipeline_cache_metadata_locked() noexcept {
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

runtime::chaining_pipeline runtime::create_pipeline_locked(
    const chaining_pipeline_key& key) {
    const bool provider_used = should_use_shader_provider_locked();
    const bool archive_receipt_required = provider_used
        || is_runtime_metal_archive_configuration_installed();
    for (const auto& entry : pipeline_cache_) {
        if (entry.key == key) {
            if (archive_receipt_required) {
                require_immutable_metal_policy_live(
                    "cached chaining pipeline retrieval");
            }
            if (archive_receipt_required
                && (entry.metal_archive_receipt == nullptr
                    || !entry.metal_archive_receipt->is_valid())) {
                throw vulkan_runtime_error(
                    "cached chaining pipeline lacks its accepted immutable Metal archive receipt");
            }
            return chaining_pipeline{
                .pipeline = entry.handles.pipeline,
                .shader_key = make_shader_key(
                    shader_kind::chain_tiled_dp_u64,
                    key.tile_size, key.local_size, 0u),
                .metal_archive_receipt =
                    entry.metal_archive_receipt.get(),
            };
        }
    }

    if (key.local_size == 0 || key.local_size > features_.max_workgroup_invocations) {
        throw vulkan_runtime_error(fmt::format(
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
        next_capacity, compute_pipelines_.capacity());
    pipeline_cache_admission_scope metadata_scope{*this};

    pipeline_entry candidate{};
    candidate.key = key;
    candidate.handles = owned_pipeline_handles{device_};
    VkShaderModule& module = candidate.handles.module;
    std::string& entry_name = candidate.entry_name;
    std::uint32_t provider_wg_x = 0;
    std::uint32_t provider_wg_y = 0;
    std::uint32_t provider_wg_z = 0;
    const shader_variant_key shader_key =
        make_shader_key(shader_kind::chain_tiled_dp_u64, key.tile_size, key.local_size, 0u);
    if (should_use_shader_provider_locked()) {
        if (!metallib_provider_capable_) {
            throw vulkan_runtime_error(
                "runtime shader provider installed but vkSetWorkgroupSizeMVK is unavailable");
        }
        runtime_shader_provider* provider = runtime_shader_provider_slot();
        auto loaded = provider->load_module(
            device_, shader_key, vkCreateShaderModule);
        if (!loaded) {
            throw vulkan_runtime_error(fmt::format(
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
            spirv = compile_shader_spirv(shader_compile_request{
                shader_kind::chain_tiled_dp_u64, key.tile_size, key.local_size});
        } catch (const std::exception& e) {
            throw vulkan_runtime_error(fmt::format("shader compile failed: {}", e.what()));
        }

        // Validate the direct-anchor ABI in the emitted SPIR-V before trusting the
        // pipeline. A stride/offset mismatch would silently corrupt the DP read of
        // every anchor; reject rather than hide it behind a conversion buffer.
        const spirv_layout_scan scan = scan_spirv_layout(spirv);
        if (!scan.has_array_stride(24)) {
            throw vulkan_runtime_error("SPIR-V Anchors array stride != 24");
        }
        if (!scan.has_struct_offsets({0u, 8u, 16u, 20u})) {
            throw vulkan_runtime_error("SPIR-V AnchorWire member offsets != {0,8,16,20}");
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
    metal_archive_pipeline_create_scope archive_scope{
        archive_receipt_required,
        features_.device_uuid,
        shader_key,
        entry_name};
    if (!archive_scope.is_ready_for_pipeline_call()) {
        auto rejected = archive_scope.finish(VK_ERROR_INITIALIZATION_FAILED);
        throw vulkan_runtime_error(fmt::format(
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
        throw vulkan_runtime_error(fmt::format(
            "immutable Metal archive receipt failed for chaining pipeline: {}",
            archive_verdict.error()));
    }
    if (pr != VK_SUCCESS) {
        throw vulkan_runtime_error(
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
        runtime_metal_archive_pipeline_audit& audit = **archive_verdict;
        audit.api_version = features_.api_version;
        audit.driver_version = features_.driver_version;
        audit.vendor_id = features_.vendor_id;
        audit.device_id = features_.device_id;
        audit.pipeline_cache_uuid = features_.pipeline_cache_uuid;
        audit.pipeline_create_ns = candidate.pipeline_create_ns;
        candidate.metal_archive_receipt.reset(
            new runtime_metal_archive_pipeline_receipt(
                std::move(audit)));
    }
    pipeline_cache_.push_back(std::move(candidate));
    try {
        bioseq_log<logging_level::debug>(
            "[vulkan_runtime] built chaining pipeline tile={} local={}",
            key.tile_size, key.local_size);
    } catch (...) {
    }
    const pipeline_entry& retained = pipeline_cache_.back();
    return chaining_pipeline{
        .pipeline = retained.handles.pipeline,
        .shader_key = shader_key,
        .metal_archive_receipt = retained.metal_archive_receipt.get(),
    };
}

// ---------------------------------------------------------------------------
// Upload / download helpers (called under mu_, command buffer already open)
// ---------------------------------------------------------------------------

void runtime::upload_locked(buffer &transfer_scratch, buffer &dst,
                            std::span<const std::byte> bytes,
                            VkAccessFlags dst_access) {
  (void)transfer_scratch; // unified-memory path has no bounce allocation
  (void)dst_access; // host-coherent + queue submit makes host writes visible
  if (bytes.empty())
    return;
  if (dst.mapped == nullptr) {
    // Vulkan chaining requires host-visible device buffers (unified memory). The
    // scratch parameter is retained in the ABI for a future discrete-GPU
    // bounce path; an invalid/default buffer is intentional today.
    throw vulkan_runtime_error(
        "device buffer is not host-visible (Vulkan chaining needs unified memory)");
  }
#ifdef BIOSEQ_TEST_INDEXES
  note_vulkan_submission_test_upload();
#endif
  DEBUG_ASSERT(bytes.size() <= dst.bytes,
               "upload exceeds destination buffer size");
  if (bytes.data() != static_cast<const std::byte *>(dst.mapped)) {
    std::memcpy(dst.mapped, bytes.data(), bytes.size());
  }
}

void runtime::download_locked(
    buffer& transfer_scratch, const buffer& src, std::span<std::byte> bytes) {
    (void)transfer_scratch;  // unified-memory path has no bounce allocation
    if (bytes.empty()) return;
    if (src.mapped == nullptr) {
        throw vulkan_runtime_error("device buffer is not host-visible (Vulkan chaining needs unified memory)");
    }
    DEBUG_ASSERT(bytes.size() <= src.bytes, "download exceeds source buffer size");
    if (bytes.data() != static_cast<std::byte*>(src.mapped)) {
        std::memcpy(bytes.data(), src.mapped, bytes.size());
    }
}

// ---------------------------------------------------------------------------
// The one hot entry point
// ---------------------------------------------------------------------------

void runtime::run_chaining_tiled_dp_roundtrip(
    const chaining_pipeline_key& key,
    buffer& transfer_scratch,
    buffer& anchors,
    buffer& ranges,
    buffer& best_scores,
    buffer& predecessors,
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
        throw vulkan_runtime_error("run_chaining_tiled_dp_roundtrip on unavailable device");
    }
    // ONE WORKGROUP PER RANGE; the kernel grid-strides
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
        {anchors.buffer_handle, 0, VK_WHOLE_SIZE},
        {ranges.buffer_handle, 0, VK_WHOLE_SIZE},
        {best_scores.buffer_handle, 0, VK_WHOLE_SIZE},
        {predecessors.buffer_handle, 0, VK_WHOLE_SIZE},
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

    chaining_push_constants push{};
    push.anchor_count = anchor_count;
    push.range_count = range_count;
    push.max_dist_x = max_dist_x;
    push.max_dist_y = max_dist_y;
    push.bandwidth = bandwidth;
    push.max_iter = max_iter;
    push.gap_penalty = gap_penalty;
    push.skip_penalty = skip_penalty;
    push.max_chain_skip = max_chain_skip;

    // record: host-write barrier -> dispatch -> shader-write->host-read barrier.
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

gpu_coordinate_chain_trace_result runtime::run_chaining_coordinate_trace_roundtrip(
    const chaining_pipeline_key& key,
    buffer& transfer_scratch,
    buffer& anchors,
    buffer& ranges,
    buffer& best_scores,
    buffer& predecessors,
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

    gpu_coordinate_chain_trace_result out{};
    if (!features_.available) {
        throw vulkan_runtime_error("run_chaining_coordinate_trace_roundtrip on unavailable device");
    }

    const std::uint32_t chain_groups =
        std::min(range_count, features_.max_workgroup_count_x);
    const VkPipeline pipeline = create_pipeline_locked(key).pipeline;
    const compute_pipeline coord_pipe =
        get_compute_pipeline_locked(shader_kind::coordinate_reduce, 128u, 1u);

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
    buffer visited = create_device_buffer(
        std::uint64_t{anchor_count} * sizeof(std::uint32_t),
        coordinate_trace_buffer_usage);
    buffer trace = create_device_buffer(
        std::uint64_t{anchor_count} * sizeof(std::uint32_t),
        coordinate_trace_buffer_usage);
    buffer result_buf = create_device_buffer(
        sizeof(gpu_coordinate_result_wire), coordinate_trace_buffer_usage);
    if (visited.mapped == nullptr || trace.mapped == nullptr || result_buf.mapped == nullptr) {
        throw vulkan_runtime_error("coordinate-only chaining requires host-visible trace buffers");
    }
    std::memset(visited.mapped, 0, static_cast<std::size_t>(visited.bytes));
    std::memset(result_buf.mapped, 0, static_cast<std::size_t>(result_buf.bytes));

    const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
        {anchors.buffer_handle, 0, VK_WHOLE_SIZE},
        {ranges.buffer_handle, 0, VK_WHOLE_SIZE},
        {best_scores.buffer_handle, 0, VK_WHOLE_SIZE},
        {predecessors.buffer_handle, 0, VK_WHOLE_SIZE},
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

    chaining_push_constants chain_push{};
    chain_push.anchor_count = anchor_count;
    chain_push.range_count = range_count;
    chain_push.max_dist_x = max_dist_x;
    chain_push.max_dist_y = max_dist_y;
    chain_push.bandwidth = bandwidth;
    chain_push.max_iter = max_iter;
    chain_push.gap_penalty = gap_penalty;
    chain_push.skip_penalty = skip_penalty;
    chain_push.max_chain_skip = max_chain_skip;

    reset_dispatch_descriptor_pool_locked();
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
        {anchors.buffer_handle, 0, VK_WHOLE_SIZE},
        {best_scores.buffer_handle, 0, VK_WHOLE_SIZE},
        {predecessors.buffer_handle, 0, VK_WHOLE_SIZE},
        {visited.buffer_handle, 0, visited.bytes},
        {trace.buffer_handle, 0, trace.bytes},
        {result_buf.buffer_handle, 0, result_buf.bytes},
    }};
    gpu_coordinate_reduce_push coord_push{
        .anchor_count = anchor_count,
        .query_len_lo = static_cast<std::uint32_t>(query_len & 0xffffffffull),
        .query_len_hi = static_cast<std::uint32_t>(query_len >> 32),
        .min_chain_score = min_chain_score,
        .min_seed_count = min_seed_count,
        .probe_limit = probe_limit,
        .reserved = 0u,
    };
    record_compute_dispatch_locked(
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
    if (out.result.is_valid != 0u) {
        if (out.result.seed_count < 0
            || static_cast<std::uint64_t>(out.result.seed_count) > anchor_count) {
            throw vulkan_runtime_error("coordinate-only chaining produced invalid trace length");
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
// Seed lookup — seeding roundtrips
// ---------------------------------------------------------------------------

namespace {

struct seed_lookup_push_constants {
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
static_assert(sizeof(seed_lookup_push_constants) == 44);

constexpr VkBufferUsageFlags k_compute_buffer_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT;

struct alignment_extension_push_constants {
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
static_assert(sizeof(alignment_extension_push_constants) == 44);
static_assert(std::is_standard_layout_v<alignment_extension_push_constants>);

struct alignment_cigar_push_constants {
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
static_assert(sizeof(alignment_cigar_push_constants) == 60);
static_assert(std::is_standard_layout_v<alignment_cigar_push_constants>);

[[nodiscard]] alignment_extension_push_constants alignment_extension_push(
    std::uint32_t job_count,
    const gpu_alignment_extension_params& params) noexcept
{
    return alignment_extension_push_constants{
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

[[nodiscard]] alignment_cigar_push_constants alignment_cigar_push(
    std::uint32_t job_count,
    const gpu_alignment_cigar_params& params) noexcept
{
    return alignment_cigar_push_constants{
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
    constexpr std::int32_t k_max_absolute_score = 1'000'000;
    if (value < 0 || value > k_max_absolute_score) {
        throw vulkan_runtime_error(fmt::format(
            "alignment extension {} {} out of range (0..{})",
            name, value, k_max_absolute_score));
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
        throw vulkan_runtime_error(fmt::format(
            "alignment extension job {} {} range [{}, {}) exceeds symbol count {}",
            job_index, label, begin, end, symbols.size()));
    }
    for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint32_t symbol = symbols[static_cast<std::size_t>(offset) + i];
        if (symbol >= alphabet_size) {
            throw vulkan_runtime_error(fmt::format(
                "alignment extension job {} {} symbol {} at local offset {} exceeds alphabet_size {}",
                job_index, label, symbol, i, alphabet_size));
        }
    }
}

void validate_alignment_extension_inputs(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const gpu_alignment_extension_job_wire> jobs,
    const gpu_alignment_extension_params& params)
{
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error("alignment extension job_count exceeds uint32");
    }
    if (query_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error("alignment extension query symbol stream exceeds uint32 offsets");
    }
    if (target_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error("alignment extension target symbol stream exceeds uint32 offsets");
    }
    if (params.max_query_len == 0
        || params.max_query_len > k_gpu_alignment_extension_max_len) {
        throw vulkan_runtime_error(fmt::format(
            "alignment extension max_query_len {} out of range (1..{})",
            params.max_query_len, k_gpu_alignment_extension_max_len));
    }
    if (params.max_target_len == 0
        || params.max_target_len > k_gpu_alignment_extension_max_len) {
        throw vulkan_runtime_error(fmt::format(
            "alignment extension max_target_len {} out of range (1..{})",
            params.max_target_len, k_gpu_alignment_extension_max_len));
    }
    if (params.max_band_width > k_gpu_alignment_extension_max_band_width) {
        throw vulkan_runtime_error(fmt::format(
            "alignment extension max_band_width {} exceeds {}",
            params.max_band_width, k_gpu_alignment_extension_max_band_width));
    }
    if (params.alphabet_size == 0 || params.alphabet_size > 256u) {
        throw vulkan_runtime_error(fmt::format(
            "alignment extension alphabet_size {} out of range (1..256)",
            params.alphabet_size));
    }
    validate_alignment_extension_score("match_score", params.match_score);
    validate_alignment_extension_score("mismatch_penalty", params.mismatch_penalty);
    validate_alignment_extension_score("gap_open_penalty", params.gap_open_penalty);
    validate_alignment_extension_score("gap_extend_penalty", params.gap_extend_penalty);
    if (params.match_score == 0) {
        throw vulkan_runtime_error("alignment extension match_score must be positive");
    }
    if (params.flags != 0 || params.reserved0 != 0) {
        throw vulkan_runtime_error("alignment extension params flags/reserved must be zero");
    }

    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const gpu_alignment_extension_job_wire& job = jobs[i];
        if (job.query_len > params.max_query_len) {
            throw vulkan_runtime_error(fmt::format(
                "alignment extension job {} query_len {} exceeds max_query_len {}",
                i, job.query_len, params.max_query_len));
        }
        if (job.target_len > params.max_target_len) {
            throw vulkan_runtime_error(fmt::format(
                "alignment extension job {} target_len {} exceeds max_target_len {}",
                i, job.target_len, params.max_target_len));
        }
        if (job.band_width > params.max_band_width) {
            throw vulkan_runtime_error(fmt::format(
                "alignment extension job {} band_width {} exceeds max_band_width {}",
                i, job.band_width, params.max_band_width));
        }
        if (job.flags != 0 || job.reserved0 != 0 || job.reserved1 != 0) {
            throw vulkan_runtime_error(fmt::format(
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

void validate_prefilter_inputs(
    const char* algorithm,
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const bioseq::prefilter::prefilter_job> jobs)
{
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error(fmt::format(
            "{} job_count exceeds uint32", algorithm));
    }
    if (query_ranks.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error(fmt::format(
            "{} query rank stream exceeds uint32 offsets", algorithm));
    }
    if (target_ranks.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error(fmt::format(
            "{} target rank stream exceeds uint32 offsets", algorithm));
    }

}

void validate_sneaky_snake_results(
    std::span<const bioseq::prefilter::prefilter_job> jobs,
    std::span<const bioseq::prefilter::prefilter_result> results,
    std::size_t query_rank_count,
    std::size_t target_rank_count)
{
    using bioseq::prefilter::prefilter_status;
    if (results.size() != jobs.size()) {
        throw vulkan_runtime_error("SneakySnake result count mismatch");
    }
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const bioseq::prefilter::prefilter_job& job = jobs[i];
        const bioseq::prefilter::prefilter_result& result =
            results[i];
        const bool in_bounds =
            bioseq::prefilter::prefilter_slice_in_bounds(
                query_rank_count, job.query_offset, job.query_length)
            && bioseq::prefilter::prefilter_slice_in_bounds(
                target_rank_count, job.target_offset, job.target_length);
        if (result.is_accepted > 1u || result.diagnostics != 0u) {
            throw vulkan_runtime_error(fmt::format(
                "SneakySnake shader returned malformed flags for job {}",
                i));
        }

        if (result.status == static_cast<std::uint32_t>(
                prefilter_status::unsupported_conservative)) {
            if ((in_bounds
                 && job.max_edits <= k_gpu_sneaky_snake_max_edits)
                || result.lower_bound != 0u || result.is_accepted != 1u) {
                throw vulkan_runtime_error(fmt::format(
                    "SneakySnake shader returned invalid unsupported result for job {}",
                    i));
            }
            continue;
        }

        if (!in_bounds || job.max_edits > k_gpu_sneaky_snake_max_edits) {
            throw vulkan_runtime_error(fmt::format(
                "SneakySnake shader failed to retain unsupported job {}",
                i));
        }
        if (result.status == static_cast<std::uint32_t>(
                prefilter_status::conservative_unequal)) {
            if (job.query_length == job.target_length) {
                throw vulkan_runtime_error(fmt::format(
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
                || result.is_accepted != expected_accepted) {
                throw vulkan_runtime_error(fmt::format(
                    "SneakySnake shader returned invalid length-difference result for job {}",
                    i));
            }
            continue;
        }
        if (result.status != static_cast<std::uint32_t>(
                prefilter_status::success)
            || job.query_length != job.target_length) {
            throw vulkan_runtime_error(fmt::format(
                "SneakySnake shader returned invalid status {} for job {}",
                result.status, i));
        }
        const std::uint32_t expected_accepted =
            result.lower_bound <= job.max_edits ? 1u : 0u;
        if (result.is_accepted != expected_accepted
            || result.lower_bound > job.max_edits + 1u) {
            throw vulkan_runtime_error(fmt::format(
                "SneakySnake shader returned invalid equal-length result for job {}",
                i));
        }
    }
}

void validate_gatekeeper_results(
    std::span<const bioseq::prefilter::prefilter_job> jobs,
    std::span<const bioseq::prefilter::prefilter_result> results,
    std::size_t query_rank_count,
    std::size_t target_rank_count)
{
    using bioseq::prefilter::prefilter_job;
    using bioseq::prefilter::prefilter_result;
    using bioseq::prefilter::prefilter_status;
    using bioseq::prefilter::gatekeeper_diagnostic_estimate;
    using bioseq::prefilter::k_gatekeeper_diagnostic_valid_mask;
    using bioseq::prefilter::is_gatekeeper_exact_guard_activated;
    using bioseq::prefilter::is_gatekeeper_guard_confirmed_rejection;
    using bioseq::prefilter::is_gatekeeper_hamming_exact_rejection;
    using bioseq::prefilter::is_gatekeeper_mask_rejection_candidate;
    if (results.size() != jobs.size()) {
        throw vulkan_runtime_error("GateKeeper result count mismatch");
    }
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const prefilter_job& job = jobs[i];
        const prefilter_result& result = results[i];
        if (result.is_accepted > 1u
            || (result.diagnostics
                & ~k_gatekeeper_diagnostic_valid_mask) != 0u) {
            throw vulkan_runtime_error(fmt::format(
                "GateKeeper shader returned malformed flags for job {}", i));
        }

        const std::uint32_t unsupported = static_cast<std::uint32_t>(
            prefilter_status::unsupported_conservative);
        const std::uint32_t unequal = static_cast<std::uint32_t>(
            prefilter_status::conservative_unequal);
        const std::uint32_t success = static_cast<std::uint32_t>(
            prefilter_status::success);
        const bool in_bounds =
            bioseq::prefilter::prefilter_slice_in_bounds(
                query_rank_count, job.query_offset, job.query_length)
            && bioseq::prefilter::prefilter_slice_in_bounds(
                target_rank_count, job.target_offset, job.target_length);
        if (!in_bounds) {
            if (result.status != unsupported || result.lower_bound != 0u
                || result.is_accepted != 1u || result.diagnostics != 0u) {
                throw vulkan_runtime_error(fmt::format(
                    "GateKeeper shader did not fail open invalid job {}", i));
            }
            continue;
        }
        if (job.query_length == 0u && job.target_length == 0u) {
            if (result.status != success || result.lower_bound != 0u
                || result.is_accepted != 1u || result.diagnostics != 0u) {
                throw vulkan_runtime_error(fmt::format(
                    "GateKeeper shader returned invalid empty result for job {}",
                    i));
            }
            continue;
        }
        if (job.query_length != job.target_length) {
            const std::uint32_t length_difference =
                job.query_length > job.target_length
                ? job.query_length - job.target_length
                : job.target_length - job.query_length;
            const std::uint32_t expected_accepted =
                length_difference <= job.max_edits ? 1u : 0u;
            if (result.status != unequal
                || result.lower_bound != length_difference
                || result.is_accepted != expected_accepted
                || result.diagnostics != 0u) {
                throw vulkan_runtime_error(fmt::format(
                    "GateKeeper shader returned invalid unequal-length result for job {}",
                    i));
            }
            continue;
        }

        const bool supported_geometry =
            job.query_length <= k_gpu_gatekeeper_max_length
            && job.max_edits <= k_gpu_gatekeeper_max_edits;
        if (!supported_geometry) {
            if (result.status != unsupported || result.lower_bound != 0u
                || result.is_accepted != 1u || result.diagnostics != 0u) {
                throw vulkan_runtime_error(fmt::format(
                    "GateKeeper shader returned invalid unsupported result for job {}",
                    i));
            }
            continue;
        }

        if (result.status != success) {
            throw vulkan_runtime_error(fmt::format(
                "GateKeeper shader returned invalid status {} for job {}",
                result.status, i));
        }

        const bool candidate = is_gatekeeper_mask_rejection_candidate(result);
        const bool guard = is_gatekeeper_exact_guard_activated(result);
        const bool confirmed = is_gatekeeper_guard_confirmed_rejection(result);
        const bool hamming = is_gatekeeper_hamming_exact_rejection(result);
        const std::uint32_t estimate =
            gatekeeper_diagnostic_estimate(result);
        if (candidate != guard || (confirmed && !guard)
            || (hamming && (candidate || guard || confirmed))
            || estimate > job.max_edits + 1u
            || ((candidate || hamming)
                && estimate != job.max_edits + 1u)
            || (!candidate && !hamming && estimate > job.max_edits)
            || (hamming && job.max_edits != 0u)) {
            throw vulkan_runtime_error(fmt::format(
                "GateKeeper shader returned inconsistent diagnostics for job {}",
                i));
        }
        if (result.is_accepted != 0u) {
            if (result.lower_bound != 0u || confirmed || hamming) {
                throw vulkan_runtime_error(fmt::format(
                    "GateKeeper shader returned invalid accepted result for job {}",
                    i));
            }
        } else if (result.lower_bound != job.max_edits + 1u
                   || (!confirmed && !hamming)) {
            throw vulkan_runtime_error(fmt::format(
                "GateKeeper shader returned unconfirmed rejection for job {}",
                i));
        }
    }
}

void validate_alignment_cigar_score(const char* name, std::int32_t value) {
    constexpr std::int32_t k_max_absolute_score = 1'000'000;
    if (value < 0 || value > k_max_absolute_score) {
        throw vulkan_runtime_error(fmt::format(
            "alignment CIGAR extension {} {} out of range (0..{})",
            name, value, k_max_absolute_score));
    }
}

void validate_alignment_cigar_inputs(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const gpu_alignment_cigar_job_wire> jobs,
    const gpu_alignment_cigar_params& params,
    std::uint32_t packed_cigar_capacity)
{
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error("alignment CIGAR extension job_count exceeds uint32");
    }
    if (query_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error(
            "alignment CIGAR extension query symbol stream exceeds uint32 offsets");
    }
    if (target_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error(
            "alignment CIGAR extension target symbol stream exceeds uint32 offsets");
    }
    if (params.alphabet_size == 0 || params.alphabet_size > 256u) {
        throw vulkan_runtime_error(fmt::format(
            "alignment CIGAR extension alphabet_size {} out of range (1..256)",
            params.alphabet_size));
    }
    if (params.max_band_width > k_gpu_alignment_extension_max_band_width) {
        throw vulkan_runtime_error(fmt::format(
            "alignment CIGAR extension max_band_width {} exceeds {}",
            params.max_band_width, k_gpu_alignment_extension_max_band_width));
    }
    if (params.tile_query == 0 || params.tile_query > k_gpu_alignment_extension_max_len
        || params.tile_target == 0
        || params.tile_target > k_gpu_alignment_extension_max_len) {
        throw vulkan_runtime_error(fmt::format(
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
        throw vulkan_runtime_error("alignment CIGAR extension match_score must be positive");
    }
    if (params.flags != 0 || params.reserved != 0) {
        throw vulkan_runtime_error("alignment CIGAR extension params flags/reserved must be zero");
    }
    if (packed_cigar_capacity == 0) {
        throw vulkan_runtime_error("alignment CIGAR extension packed CIGAR capacity is zero");
    }

    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const gpu_alignment_cigar_job_wire& job = jobs[i];
        if (job.query_len > k_gpu_alignment_simple_cigar_max_len
            || job.target_len > k_gpu_alignment_simple_cigar_max_len) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR extension job {} length {}x{} exceeds {}",
                i, job.query_len, job.target_len,
                k_gpu_alignment_simple_cigar_max_len));
        }
        if (job.band_width > params.max_band_width) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR extension job {} band_width {} exceeds max_band_width {}",
                i, job.band_width, params.max_band_width));
        }
        if (job.cigar_capacity == 0) {
            throw vulkan_runtime_error(fmt::format(
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
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR extension job {} traceback capacity {} below required {}",
                i, job.traceback_tile_count, traceback_cells));
        }
        if (traceback_end > std::numeric_limits<std::uint32_t>::max()) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR extension job {} traceback range [{}, {}) exceeds uint32",
                i, job.traceback_tile_offset, traceback_end));
        }
        const std::uint64_t cigar_end =
            static_cast<std::uint64_t>(job.cigar_offset)
            + static_cast<std::uint64_t>(job.cigar_capacity);
        if (cigar_end > packed_cigar_capacity) {
            throw vulkan_runtime_error(fmt::format(
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

/// @brief Device byte footprint for `count` packed minimizers of the given word kind.
/// @return The computed result.
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

/// @brief Host byte footprint for `count` minimizers (one packed element each).
/// @return The computed result.
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

runtime::compute_pipeline runtime::get_compute_pipeline_locked(
    shader_kind kind, std::uint32_t local_size, std::uint32_t max_span)
{
    const bool provider_used = should_use_shader_provider_locked();
    const bool archive_receipt_required = provider_used
        || is_runtime_metal_archive_configuration_installed();
    for (auto& entry : compute_pipelines_) {
        if (entry.kind == kind && entry.local_size == local_size
            && entry.max_span == max_span) {
            if (archive_receipt_required) {
                require_immutable_metal_policy_live(
                    "cached compute pipeline retrieval");
            }
            if (archive_receipt_required
                && (entry.metal_archive_receipt == nullptr
                    || !entry.metal_archive_receipt->is_valid())) {
                throw vulkan_runtime_error(
                    "cached compute pipeline lacks its accepted immutable Metal archive receipt");
            }
            return entry.view();
        }
    }
    const shader_variant_key shader_key = make_shader_key(kind, 384u, local_size, max_span);
    shader_compile_request geometry_request{};
    geometry_request.kind = kind;
    geometry_request.tile_size = 384u;
    geometry_request.local_size = local_size;
    geometry_request.max_span = max_span;
    const shader_workgroup_size workgroup =
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
        throw vulkan_runtime_error(fmt::format(
            "seed lookup workgroup {}x{}x{} (key local_size {}) exceeds device "
            "limits maxComputeWorkGroupSize={}x{}x{} or "
            "maxComputeWorkGroupInvocations={}",
            workgroup.x, workgroup.y, workgroup.z, local_size,
            features_.max_workgroup_size_x, features_.max_workgroup_size_y,
            features_.max_workgroup_size_z,
            features_.max_workgroup_invocations));
    }

    const descriptor_layout_kind layout_kind = descriptor_layout_for(kind);
    const std::uint32_t binding_count = descriptor_binding_count(layout_kind);
    const std::uint32_t push_size = push_constant_size(kind);

    const std::size_t next_capacity = compute_pipelines_.size()
            < compute_pipelines_.capacity()
        ? compute_pipelines_.capacity()
        : std::max<std::size_t>(
            compute_pipelines_.size() + 1u,
            compute_pipelines_.capacity() == 0u
                ? 1u : compute_pipelines_.capacity() * 2u);
    admit_pipeline_cache_growth_locked(
        pipeline_cache_.capacity(), next_capacity);
    pipeline_cache_admission_scope metadata_scope{*this};

    owned_compute_pipeline entry;
    entry.handles = owned_pipeline_handles{device_};
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
             "vkCreateDescriptorSetLayout(compute)");

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
             "vkCreatePipelineLayout(compute)");

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
             "vkCreateDescriptorPool(compute)");

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = entry.handles.descriptor_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &entry.handles.set_layout;
    check_vk(vkAllocateDescriptorSets(device_, &set_alloc, &entry.descriptor_set),
             "vkAllocateDescriptorSets(compute)");

    if (provider_used) {
        if (!metallib_provider_capable_) {
            throw vulkan_runtime_error(
                "runtime shader provider installed but vkSetWorkgroupSizeMVK is unavailable");
        }
        runtime_shader_provider* provider = runtime_shader_provider_slot();
        auto loaded = provider->load_module(
            device_, shader_key, vkCreateShaderModule);
        if (!loaded) {
            throw vulkan_runtime_error(fmt::format(
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
            shader_compile_request request{};
            request.kind = kind;
            request.tile_size = 384;
            request.local_size = local_size;
            request.max_span = max_span;
            spirv = compile_shader_spirv(request);
        } catch (const std::exception& compile_error) {
            throw vulkan_runtime_error(fmt::format("seed lookup shader compile failed: {}",
                                                 compile_error.what()));
        }

        // Pin the SSBO wire ABI: a sort-range-stride mismatch would silently
        // corrupt direct sort_range_wire reads.
        const spirv_layout_scan scan = scan_spirv_layout(spirv);
        if (kind == shader_kind::seed_lookup_anchors
            && (!scan.has_binding(0) || !scan.has_binding(1)
                || !scan.has_binding(2) || !scan.has_binding(3)
                || !scan.has_binding(4) || !scan.has_binding(5)
                || !scan.has_array_stride(24) || !scan.has_array_stride(8)
                || !scan.has_struct_offsets({0u, 8u, 16u, 20u}))) {
            throw vulkan_runtime_error("SPIR-V seed lookup anchor/key ABI mismatch");
        }
        if (kind == shader_kind::sort_anchors_bitonic && !scan.has_array_stride(8)) {
            throw vulkan_runtime_error("SPIR-V SortRange array stride != 8");
        }
        if (kind == shader_kind::generate_units &&
             (!scan.has_binding(0) || !scan.has_binding(1) ||
             !scan.has_array_stride(max_span / 8u))) {
            throw vulkan_runtime_error("SPIR-V generate_units layout mismatch");
        }
        if (kind == shader_kind::align_extend_banded_affine &&
            (!scan.has_binding(0) || !scan.has_binding(1) ||
             !scan.has_binding(2) || !scan.has_binding(3) ||
             !scan.has_array_stride(4) || !scan.has_array_stride(16) ||
             !scan.has_array_stride(32))) {
            throw vulkan_runtime_error("SPIR-V alignment extension layout mismatch");
        }
        if (kind == shader_kind::align_extend_simple_cigar &&
            (!scan.has_binding(0) || !scan.has_binding(1) ||
             !scan.has_binding(2) || !scan.has_binding(3) ||
             !scan.has_binding(4) || !scan.has_binding(5) ||
             !scan.has_array_stride(4) ||
             !scan.has_array_stride(40) || !scan.has_array_stride(48))) {
            throw vulkan_runtime_error("SPIR-V alignment CIGAR extension layout mismatch");
        }
        if ((kind == shader_kind::sneaky_snake_rank_batch
             || is_gatekeeper_rank_batch_kind(kind)) &&
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
                 std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}))) {
            throw vulkan_runtime_error(
                "SPIR-V rank-prefilter batch layout mismatch");
        }

        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = spirv.size() * sizeof(std::uint32_t);
        module_info.pCode = spirv.data();
        check_vk(vkCreateShaderModule(
                     device_, &module_info, nullptr,
                     &entry.handles.module),
                 "vkCreateShaderModule(compute)");
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
    metal_archive_pipeline_create_scope archive_scope{
        archive_receipt_required,
        features_.device_uuid,
        shader_key,
        entry.entry_name};
    if (!archive_scope.is_ready_for_pipeline_call()) {
        auto rejected = archive_scope.finish(VK_ERROR_INITIALIZATION_FAILED);
        throw vulkan_runtime_error(fmt::format(
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
        throw vulkan_runtime_error(fmt::format(
            "immutable Metal archive receipt failed for compute pipeline: {}",
            archive_verdict.error()));
    }
    if (pr != VK_SUCCESS) {
        throw vulkan_runtime_error(fmt::format(
            "vkCreateComputePipelines(compute): VkResult {}", static_cast<int>(pr)));
    }
    if (archive_verdict->has_value()) {
        runtime_metal_archive_pipeline_audit& audit = **archive_verdict;
        audit.api_version = features_.api_version;
        audit.driver_version = features_.driver_version;
        audit.vendor_id = features_.vendor_id;
        audit.device_id = features_.device_id;
        audit.pipeline_cache_uuid = features_.pipeline_cache_uuid;
        audit.pipeline_create_ns = entry.pipeline_create_ns;
        entry.metal_archive_receipt.reset(
            new runtime_metal_archive_pipeline_receipt(
                std::move(audit)));
    }

    compute_pipelines_.push_back(std::move(entry));
    try {
        bioseq_log<logging_level::debug>(
            "[vulkan_runtime] built compute pipeline kind={} local={} max_span={}",
            static_cast<int>(kind), local_size, max_span);
    } catch (...) {
    }
    return compute_pipelines_.back().view();
}

void runtime::bind_compute_pipeline_descriptors_locked(
    const compute_pipeline& entry, std::span<const VkBuffer> buffers)
{
    DEBUG_ASSERT(buffers.size() == entry.binding_count,
                 "seed lookup descriptor binding count mismatch");
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

seed_lookup_result runtime::run_seed_lookup_roundtrip(
    runtime_seed_lookup_inputs inputs,
    std::span<const std::uint32_t> prefix_counts,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::uint64_t exact_anchor_count,
    const seed_anchor_sort_plan& sort_plan,
    std::uint64_t expected_vulkan_allocation_bytes)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_seed_lookup_roundtrip on unavailable device");
    }
    if (prefix_counts.size() < 2 || exact_anchor_count == 0) {
        if (expected_vulkan_allocation_bytes != 0u) {
            throw vulkan_runtime_error(
                "zero-anchor seed lookup carried a Vulkan allocation plan");
        }
        seed_lookup_result empty_result{};
        empty_result.anchor_order_audit = audit_seed_anchor_canonical_order({}, 0u);
        return empty_result;
    }
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw vulkan_runtime_error("seed lookup missing resident index buffers");
    }
    if (static_cast<std::uint64_t>(inputs.position_count) * 8u
        > inputs.positions->bytes) {
        throw vulkan_runtime_error(
            "seed lookup resident position range is malformed");
    }
    const std::uint64_t slot_count_from_bytes = inputs.slots->bytes / 16u;
    if (inputs.bucket_count != 0u) {
        if (inputs.slot_mask != 0u
            || inputs.bucket_count > slot_count_from_bytes) {
            throw vulkan_runtime_error(
                "seed lookup sorted-dense index representation is malformed");
        }
    } else {
        const std::uint64_t open_capacity =
            static_cast<std::uint64_t>(inputs.slot_mask) + 1u;
        if (inputs.slot_mask == 0u
            || !std::has_single_bit(open_capacity)
            || open_capacity > slot_count_from_bytes) {
            throw vulkan_runtime_error(
                "seed lookup open-addressed index representation is malformed");
        }
    }
    const seed_anchor_sort_plan expected_sort_plan = make_seed_anchor_sort_plan(
        exact_anchor_count,
        inputs.target_len,
        inputs.target_position_offset,
        gpu_anchor_sort_capacity_,
        features_.max_workgroup_count_x,
        features_.max_workgroup_invocations,
        features_.max_workgroup_size_x,
        features_.max_storage_buffer_range);
    if (!sort_plan.is_valid() || sort_plan != expected_sort_plan
        || sort_plan.backend == seed_anchor_sort_backend::none) {
        throw vulkan_runtime_error(
            "seed lookup sort plan does not match the runtime/device contract");
    }
    if (prefix_counts.back() != exact_anchor_count) {
        throw vulkan_runtime_error("seed lookup prefix total disagrees with anchor count");
    }
    const std::uint64_t minimizer_count = prefix_counts.size() - 1u;
    constexpr std::uint32_t seed_local = 128u;
    const std::uint64_t seed_groups =
        (minimizer_count + seed_local - 1u) / seed_local;
    if (seed_groups > features_.max_workgroup_count_x) {
        throw vulkan_runtime_error("seed lookup dispatch exceeds maxComputeWorkGroupCount[0]");
    }

    const compute_pipeline seed_pipe =
        get_compute_pipeline_locked(shader_kind::seed_lookup_anchors, seed_local, 256u);

    buffer scratch = create_host_buffer(4u, k_compute_buffer_usage);

    buffer uploaded_min_words;
    const buffer* min_words = inputs.device_minimizer_words;
    if (min_words == nullptr) {
        const std::uint64_t min_bytes = std::max<std::uint64_t>(
            minimizer_device_bytes(inputs.minimizer_word_kind,
                                   inputs.minimizer_words32_per_record, minimizer_count), 4u);
        uploaded_min_words = create_device_buffer(min_bytes, k_compute_buffer_usage);
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

    buffer prefix_buf = create_device_buffer(
        std::max<std::uint64_t>(prefix_counts.size() * sizeof(std::uint32_t), 4u),
        k_compute_buffer_usage);
    upload_locked(scratch, prefix_buf, std::as_bytes(prefix_counts), VK_ACCESS_SHADER_READ_BIT);

    const std::uint64_t anchor_bytes = sort_plan.anchor_bytes;
    buffer anchors_buf = create_device_buffer(anchor_bytes, k_compute_buffer_usage);
    buffer anchor_keys_buf = create_device_buffer(
        sort_plan.key_bytes, k_compute_buffer_usage);
    buffer ranges_buf;
    std::optional<compute_pipeline> sort_pipe;
    buffer anchors_scratch_buf;
    buffer anchor_keys_scratch_buf;
    buffer histogram_buf;
    if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
        ranges_buf = create_device_buffer(sizeof(sort_range_wire), k_compute_buffer_usage);
        const sort_range_wire range{
            0u, static_cast<std::uint32_t>(exact_anchor_count)};
        upload_locked(scratch, ranges_buf,
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(&range), sizeof(range)},
            VK_ACCESS_SHADER_READ_BIT);
        sort_pipe = get_compute_pipeline_locked(
            shader_kind::sort_anchors_bitonic,
            gpu_anchor_sort_capacity_,
            256u);
    } else {
        anchors_scratch_buf = create_device_buffer(
            sort_plan.anchor_bytes, k_compute_buffer_usage);
        anchor_keys_scratch_buf = create_device_buffer(
            sort_plan.key_bytes, k_compute_buffer_usage);
        histogram_buf = create_device_buffer(
            sort_plan.histogram_bytes, k_compute_buffer_usage);
    }

    std::uint64_t actual_vulkan_allocation_bytes = 0u;
    const auto add_allocation = [&actual_vulkan_allocation_bytes](
            const buffer& buffer) noexcept {
        actual_vulkan_allocation_bytes = bioseq::common::saturating_add(
            actual_vulkan_allocation_bytes,
            buffer.allocation_bytes);
    };
    add_allocation(scratch);
    add_allocation(uploaded_min_words);
    add_allocation(prefix_buf);
    add_allocation(anchors_buf);
    add_allocation(anchor_keys_buf);
    add_allocation(ranges_buf);
    add_allocation(anchors_scratch_buf);
    add_allocation(anchor_keys_scratch_buf);
    add_allocation(histogram_buf);
    if (actual_vulkan_allocation_bytes
        != expected_vulkan_allocation_bytes) {
        throw vulkan_runtime_error(fmt::format(
            "seed lookup Vulkan allocation requirement mismatch "
            "(planned={} actual={})",
            expected_vulkan_allocation_bytes,
            actual_vulkan_allocation_bytes));
    }

    {
        const std::array<VkBuffer, 6> seed_bufs{
            min_words->buffer_handle, inputs.slots->buffer_handle, inputs.positions->buffer_handle,
            prefix_buf.buffer_handle, anchors_buf.buffer_handle, anchor_keys_buf.buffer_handle};
        bind_compute_pipeline_descriptors_locked(seed_pipe, seed_bufs);
        if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
            const std::array<VkBuffer, 2> sort_bufs{anchors_buf.buffer_handle, ranges_buf.buffer_handle};
            bind_compute_pipeline_descriptors_locked(*sort_pipe, sort_bufs);
        }
    }

    if (sort_plan.backend == seed_anchor_sort_backend::radix) {
        reset_dispatch_descriptor_pool_locked();
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

    const seed_lookup_push_constants seed_push{
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
    if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, sort_pipe->pipeline);
        vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                sort_pipe->pipeline_layout, 0, 1, &sort_pipe->descriptor_set, 0, nullptr);
        vkCmdDispatch(command_buffer_, 1, 1, 1);
    } else {
        const gpu_arena_slice anchors{
            &anchors_buf, 0u, sort_plan.anchor_bytes};
        const gpu_arena_slice keys{
            &anchor_keys_buf, 0u, sort_plan.key_bytes};
        const gpu_arena_slice anchors_scratch{
            &anchors_scratch_buf, 0u, sort_plan.anchor_bytes};
        const gpu_arena_slice keys_scratch{
            &anchor_keys_scratch_buf, 0u, sort_plan.key_bytes};
        const gpu_arena_slice histogram{
            &histogram_buf, 0u, sort_plan.histogram_bytes};
        const gpu_sort_plan radix_plan{
            exact_anchor_count, 8u, 24u, 8u,
            sort_plan.radix_pass_count};
        record_radix_sort_locked(
            command_buffer_, keys, anchors, keys_scratch, anchors_scratch,
            histogram, radix_plan, sort_plan.num_workgroups,
            sort_plan.elements_per_workgroup, 3u,
            radix_result_in_scratch);
        if (radix_result_in_scratch
            != is_seed_radix_result_in_scratch(sort_plan.radix_pass_count)) {
            throw vulkan_runtime_error("seed radix sort result parity mismatch");
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

    seed_lookup_result result;
    result.count = exact_anchor_count;
    result.actual_vulkan_allocation_bytes =
        actual_vulkan_allocation_bytes;
    result.seed_lookup_dispatches = 1u;
    result.open_addressed_lookup_dispatches =
        inputs.bucket_count == 0u ? 1u : 0u;
    result.sorted_dense_lookup_dispatches =
        inputs.bucket_count != 0u ? 1u : 0u;
    result.host_bytes.resize(static_cast<std::size_t>(anchor_bytes));
    const buffer& sorted_anchors = radix_result_in_scratch
        ? anchors_scratch_buf : anchors_buf;
    download_locked(scratch, sorted_anchors, std::span<std::byte>{
        result.host_bytes.data(), result.host_bytes.size()});
    const auto tie_normalization = normalize_seed_anchor_equal_ref_ties(
        std::span<std::byte>{
            result.host_bytes.data(), result.host_bytes.size()},
        result.count);
    if (!tie_normalization.gpu_ref_order_checked
        || !tie_normalization.gpu_ref_order_accepted) {
        throw vulkan_runtime_error(
            "seed lookup GPU result is not ordered by Anchor ref_bits");
    }
    result.canonical_tie_normalization_runs =
        tie_normalization.canonical_tie_run_count;
    result.canonical_tie_normalization_anchors =
        tie_normalization.canonical_tie_anchor_count;
    result.anchor_order_audit = audit_seed_anchor_canonical_order(
        std::span<const std::byte>{
            result.host_bytes.data(), result.host_bytes.size()},
        result.count);
    if (!result.anchor_order_audit.canonical_order_checked
        || !result.anchor_order_audit.canonical_order_accepted
        || result.anchor_order_audit.anchor_count != result.count) {
        throw vulkan_runtime_error(
            "seed lookup result is not in canonical Anchor order");
    }

    if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
        result.sort_stats.bitonic_sort_dispatches = 1u;
    } else {
        result.sort_stats.radix_sort_operations = 1u;
        result.sort_stats.radix_anchor_count = exact_anchor_count;
        result.sort_stats.radix_passes = sort_plan.radix_pass_count;
        result.sort_stats.radix_histogram_dispatches =
            sort_plan.radix_pass_count;
        result.sort_stats.radix_scan_dispatches = sort_plan.radix_pass_count;
        result.sort_stats.radix_scatter_dispatches =
            sort_plan.radix_pass_count;
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

// ===========================================================================
// GPU frontend — arena + frontend implementation
// ===========================================================================

namespace {

constexpr VkBufferUsageFlags k_gpu_arena_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

struct gpu_sketch_batch_arena_request {
    bool valid = false;
    std::uint32_t num_blocks = 0;
    std::uint64_t requested_bytes = 0;
};

/// @brief Authoritative eight-slice request shared by admission planning and the
/// dispatch itself: sequence, LUT, tile records, counts, offsets, scan blocks,
/// counters, and pair-value scratch.
/// @return The computed result.
[[nodiscard]] gpu_sketch_batch_arena_request
make_sketch_batch_arena_request(
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
        bioseq::common::ceil_div(tile_count, k_prefix_scan_block_size),
        1u);
    const auto append_slice = [](
        std::uint64_t total,
        std::uint64_t bytes) noexcept -> std::uint64_t {
        const std::uint64_t aligned_bytes =
            bioseq::common::checked_align_up(
                std::max<std::uint64_t>(bytes, 4u),
                k_gpu_buffer_offset_alignment)
                .value_or((std::numeric_limits<std::uint64_t>::max)());
        return bioseq::common::saturating_add(total, aligned_bytes);
    };
    const std::uint64_t tile_bytes = bioseq::common::saturating_mul(
        tile_count,
        sizeof(gpu_sketch_tile_wire));
    const std::uint64_t tile_u32_bytes = bioseq::common::saturating_mul(
        tile_count,
        sizeof(std::uint32_t));
    const std::uint64_t block_bytes = bioseq::common::saturating_mul(
        num_blocks,
        sizeof(std::uint32_t));
    std::uint64_t requested_bytes = 0u;
    requested_bytes = append_slice(requested_bytes, packed_sequence_bytes);
    requested_bytes = append_slice(requested_bytes, sizeof(vulkan_rank_lut));
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

using sketch_tile_type = gpu_sketch_tile_wire;
static_assert(sizeof(sketch_tile_type) == 24);

struct sketch_push_constants {
    std::uint32_t emit_mode, window_size, kmer_size, hash_bits, hash_shift, pos_bits;
    std::uint32_t word_kind, words32_per_minimizer, span_value, alphabet_kind;
    std::uint32_t unit_bits, bits_per_char, letters_per_unit, char_mask;
    std::uint32_t tile_count, emit_pairs, query_gate_mask, query_gate_word_count;
};
static_assert(sizeof(sketch_push_constants) == 72);

struct scan_push_constants {
    std::uint32_t element_count, block_size, num_blocks, pass;
};
static_assert(sizeof(scan_push_constants) == 16);

struct seed_push_constants {
    std::uint32_t words32_per_minimizer, word_kind, hash_shift, pos_bits, kmer_size;
    std::uint32_t slot_mask, max_occ, emit_mode, record_count, bucket_count;
    std::uint32_t target_pos_offset_lo, target_pos_offset_hi, target_ref_bits;
};
static_assert(sizeof(seed_push_constants) == 52);

struct gpu_anchor_sort_key_plan {
    std::uint32_t target_ref_bits = 63u;
    std::uint32_t pass_count = 8u;
};

[[nodiscard]] gpu_anchor_sort_key_plan make_anchor_sort_key_plan(
    std::uint64_t target_len,
    std::uint64_t target_position_offset) noexcept
{
    const auto pass_count = runtime_grouped_soa_radix_pass_count(
        target_position_offset, target_len);
    if (!pass_count.has_value()) {
        return gpu_anchor_sort_key_plan{};
    }

    const std::uint64_t max_target_coordinate =
        target_position_offset + target_len - 1u;
    const std::uint32_t target_ref_bits = std::max<std::uint32_t>(
        1u, static_cast<std::uint32_t>(std::bit_width(max_target_coordinate)));
    return gpu_anchor_sort_key_plan{
        .target_ref_bits = target_ref_bits,
        .pass_count = *pass_count,
    };
}

struct sort_push_constants {
    std::uint32_t element_count, radix_shift, num_workgroups, elems_per_wg, value_words;
};
static_assert(sizeof(sort_push_constants) == 20);

struct index_push_constants {
    std::uint32_t element_count, slot_mask, slot_capacity, num_buckets, probe_cap, pass;
};
static_assert(sizeof(index_push_constants) == 24);

struct host_query_record {
    std::uint32_t min_offset, min_count, query_len, flags;
};
static_assert(sizeof(host_query_record) == 16);

/// @brief Full compute→compute memory barrier between dependent GPU frontend dispatches.
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

}  // namespace

gpu_sketch_batch_allocation_requirements
runtime::sketch_batch_allocation_requirements(
    std::uint64_t packed_sequence_bytes,
    std::uint64_t tile_count,
    std::uint32_t minimizer_word_kind,
    std::uint32_t minimizer_words32,
    std::uint64_t max_minimizers)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error(
            "sketch_batch_allocation_requirements on unavailable device");
    }
    const bool minimizer_shape_valid =
        (minimizer_word_kind == 0u && minimizer_words32 == 0u)
        || (minimizer_word_kind == 1u && minimizer_words32 == 1u)
        || (minimizer_word_kind == 2u && minimizer_words32 == 2u)
        || (minimizer_word_kind == 3u && minimizer_words32 == 4u);
    const gpu_sketch_batch_arena_request arena_request =
        make_sketch_batch_arena_request(
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
            k_gpu_arena_usage);
    const std::uint64_t current_arena_allocation = gpu_arena_
        ? gpu_arena_->device_buffer().allocation_bytes
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
                k_compute_buffer_usage),
    };
}

buffer runtime::run_generate_units_roundtrip(
    std::uint32_t word_bits,
    std::span<const std::uint32_t> cdf,
    generate_units_push push,
    std::uint64_t out_block_bytes) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_generate_units_roundtrip on unavailable device");
    }

    constexpr std::uint32_t local = 256u;
    const compute_pipeline pipe =
        get_compute_pipeline_locked(shader_kind::generate_units, local, word_bits);

    buffer out = create_device_buffer(
        std::max<std::uint64_t>(out_block_bytes, 4u), k_compute_buffer_usage);
    buffer cdf_buf = create_device_buffer(
        std::max<std::uint64_t>(
            static_cast<std::uint64_t>(cdf.size()) * sizeof(std::uint32_t), 4u),
        k_compute_buffer_usage);
    buffer scratch = create_host_buffer(4u, k_compute_buffer_usage);
    upload_locked(scratch, cdf_buf, std::as_bytes(cdf), VK_ACCESS_SHADER_READ_BIT);

    const std::array<VkBuffer, 2> buffers{out.buffer_handle, cdf_buf.buffer_handle};
    bind_compute_pipeline_descriptors_locked(pipe, buffers);

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

void runtime::download_slice(
    const buffer& src,
    std::uint64_t src_byte_offset,
    std::span<std::byte> dst) {
    if (dst.empty()) {
        return;
    }
    if (src.mapped == nullptr) {
        throw vulkan_runtime_error("download_slice: device buffer not host-visible");
    }
    if (src_byte_offset > src.bytes ||
        dst.size() > static_cast<std::size_t>(src.bytes - src_byte_offset)) {
        throw vulkan_runtime_error("download_slice: out of range");
    }
    DEBUG_ASSERT(src_byte_offset <= src.bytes && dst.size() <= src.bytes - src_byte_offset,
                 "download_slice: out of range");
    std::memcpy(
        dst.data(),
        static_cast<const std::byte*>(src.mapped) + src_byte_offset,
        dst.size());
}

// --- Arena -----------------------------------------------------------------

gpu_arena& runtime::reusable_gpu_arena() {
    if (!gpu_arena_) {
        gpu_arena_.emplace(*this);
    }
    return *gpu_arena_;
}

bool gpu_arena::ensure_capacity(
    std::uint64_t device_bytes, std::uint64_t upload_bytes, std::uint64_t download_bytes)
{
    // Only called at op start (cursors already at their rewound baseline). Grows
    // a buffer geometrically when needed and resets just that buffer's cursor;
    // never grows mid-operation while slices are live.
    try {
        if (device_bytes > device_capacity_) {
            // Operation starts only from a rewound mark. Release the obsolete
            // arena before allocating its exact replacement so growth never
            // creates an unadmitted old+new overlap or geometric 2x tail.
            device_.reset();
            device_capacity_ = 0u;
            device_ = runtime_->create_device_buffer(
                device_bytes, k_gpu_arena_usage);
            if (device_.mapped == nullptr) return false;  // GPU frontend needs unified memory.
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
    } catch (const vulkan_runtime_error&) {
        return false;
    }
    return true;
}

bool gpu_arena::release_idle_capacity() noexcept {
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

gpu_arena_slice gpu_arena::allocate_device(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t a = std::max<std::uint64_t>(alignment, k_gpu_buffer_offset_alignment);
    const std::uint64_t offset = align_up(device_cursor_, a);
    if (offset + bytes > device_capacity_) {
        return gpu_arena_slice{};  // caller checks .is_valid()
    }
    device_cursor_ = offset + bytes;
    return gpu_arena_slice{&device_, offset, bytes};
}

gpu_arena_slice gpu_arena::allocate_upload(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t offset = align_up(upload_cursor_, std::max<std::uint64_t>(alignment, 16u));
    if (offset + bytes > upload_capacity_) return gpu_arena_slice{};
    upload_cursor_ = offset + bytes;
    return gpu_arena_slice{&upload_, offset, bytes};
}

gpu_arena_slice gpu_arena::allocate_download(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t offset = align_up(download_cursor_, std::max<std::uint64_t>(alignment, 16u));
    if (offset + bytes > download_capacity_) return gpu_arena_slice{};
    download_cursor_ = offset + bytes;
    return gpu_arena_slice{&download_, offset, bytes};
}

void gpu_arena::bind_storage_slice(
    VkWriteDescriptorSet& write, VkDescriptorBufferInfo& info,
    std::uint32_t binding, VkDescriptorSet dst_set, const gpu_arena_slice& slice) const noexcept
{
    info = VkDescriptorBufferInfo{slice.buffer->buffer_handle, slice.offset, slice.size};
    write = VkWriteDescriptorSet{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = dst_set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
}

// --- GPU frontend descriptor pool + dispatch recorder ---------------------------

void runtime::ensure_dispatch_descriptor_pool_locked() {
    if (dispatch_descriptor_pool_ != VK_NULL_HANDLE) return;
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    size.descriptorCount = 4096u * 9u;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = 4096u;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    check_vk(vkCreateDescriptorPool(device_, &info, nullptr, &dispatch_descriptor_pool_),
             "vkCreateDescriptorPool(gpu)");
}

void runtime::reset_dispatch_descriptor_pool_locked() {
    ensure_dispatch_descriptor_pool_locked();
    check_vk(vkResetDescriptorPool(device_, dispatch_descriptor_pool_, 0), "vkResetDescriptorPool(gpu)");
}

VkDescriptorSet runtime::allocate_dispatch_descriptor_set_locked(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = dispatch_descriptor_pool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    check_vk(vkAllocateDescriptorSets(device_, &info, &set), "vkAllocateDescriptorSets(gpu)");
    return set;
}

void runtime::bind_descriptors_offsets_locked(
    const compute_pipeline& entry, std::span<const VkDescriptorBufferInfo> infos)
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

void runtime::record_compute_dispatch_locked(
    VkCommandBuffer cmd, const compute_pipeline& pipe,
    std::span<const VkDescriptorBufferInfo> infos,
    const void* push, std::uint32_t push_size, std::uint32_t group_count,
    runtime_pipeline_use_collector* collector,
    std::size_t collector_job_index)
{
    if (group_count == 0) return;
    VkDescriptorSet set = allocate_dispatch_descriptor_set_locked(pipe.set_layout);
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

void runtime::record_radix_sort_locked(
    VkCommandBuffer cmd,
    const gpu_arena_slice& keys, const gpu_arena_slice& values,
    const gpu_arena_slice& keys_scratch, const gpu_arena_slice& values_scratch,
    const gpu_arena_slice& histogram,
    const gpu_sort_plan& plan, std::uint32_t num_workgroups,
    std::uint32_t elems_per_wg, std::uint32_t value_words,
    bool& result_in_scratch,
    runtime_pipeline_use_collector* collector,
    std::size_t collector_job_index)
{
    const compute_pipeline hist_pipe =
        get_compute_pipeline_locked(shader_kind::radix_sort_histogram, 1, 1);
    const compute_pipeline scan_pipe =
        get_compute_pipeline_locked(shader_kind::radix_sort_scan, 1, 1);
    const compute_pipeline scatter_pipe =
        get_compute_pipeline_locked(shader_kind::radix_sort_scatter, 1, 1);
    if (collector != nullptr) {
        const auto prepare = [collector](const compute_pipeline& pipe) {
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
        const gpu_arena_slice& cur_keys = in_scratch ? keys_scratch : keys;
        const gpu_arena_slice& cur_vals = in_scratch ? values_scratch : values;
        const gpu_arena_slice& out_keys = in_scratch ? keys : keys_scratch;
        const gpu_arena_slice& out_vals = in_scratch ? values : values_scratch;

        sort_push_constants push{
            static_cast<std::uint32_t>(plan.element_count),
            pass * plan.radix_bits, num_workgroups, elems_per_wg, value_words};

        const std::array<VkDescriptorBufferInfo, 5> infos{{
            {cur_keys.buffer->buffer_handle, cur_keys.offset, cur_keys.size},
            {cur_vals.buffer->buffer_handle, cur_vals.offset, cur_vals.size},
            {out_keys.buffer->buffer_handle, out_keys.offset, out_keys.size},
            {out_vals.buffer->buffer_handle, out_vals.offset, out_vals.size},
            {histogram.buffer->buffer_handle, histogram.offset, histogram.size},
        }};
        record_compute_dispatch_locked(
            cmd, hist_pipe, infos, &push, sizeof(push), num_workgroups,
            collector, collector_job_index);
        record_compute_barrier(cmd);
        record_compute_dispatch_locked(
            cmd, scan_pipe, infos, &push, sizeof(push), 1,
            collector, collector_job_index);
        record_compute_barrier(cmd);
        record_compute_dispatch_locked(
            cmd, scatter_pipe, infos, &push, sizeof(push), num_workgroups,
            collector, collector_job_index);
        record_compute_barrier(cmd);
        in_scratch = !in_scratch;
    }
    result_in_scratch = in_scratch;
}

void runtime::record_stable_pair_sort_by_value_then_key_locked(
    VkCommandBuffer command_buffer,
    sorted_pair_device_views& views,
    gpu_arena_slice hist,
    gpu_sort_plan value_sort_plan,
    gpu_sort_plan key_sort_plan,
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

    const gpu_arena_slice key_after_value_sort =
        value_sorted_in_scratch ? views.keys_scratch : views.keys;
    const gpu_arena_slice value_after_value_sort =
        value_sorted_in_scratch ? views.values_scratch : views.values;
    const gpu_arena_slice key_scratch_after_value_sort =
        value_sorted_in_scratch ? views.keys : views.keys_scratch;
    const gpu_arena_slice value_scratch_after_value_sort =
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

/// @brief Pick (num_workgroups, elems_per_wg) for a device sort/scan over `n` elements,
/// keeping the workgroup count under the device dispatch limit.
struct workgroup_plan { std::uint32_t num_workgroups; std::uint32_t elems_per_wg; };
[[nodiscard]] workgroup_plan plan_workgroups(std::uint64_t n, std::uint32_t max_wg) {
    if (n == 0) return workgroup_plan{0u, 1u};
    // Small per-workgroup chunks maximise concurrency for the 1-lane histogram
    // and stable scatter; the parallel scan keeps the per-bin reduction cheap
    // even with many workgroups.
    std::uint32_t elems_per_wg = 512u;
    std::uint32_t num_wg = bioseq::common::ceil_div(n, elems_per_wg);
    if (num_wg > max_wg) {
        num_wg = max_wg;
        elems_per_wg = bioseq::common::ceil_div(n, num_wg);
    }
    return workgroup_plan{num_wg, elems_per_wg};
}

/// @brief Next power-of-two open-addressed slot capacity for `distinct` buckets,
/// matching `indexing::gpu_slot_capacity` (load factor < 0.5).
/// @return The computed result.
[[nodiscard]] std::uint32_t sketch_slot_capacity(std::uint64_t distinct) {
    const std::uint64_t wanted = bioseq::common::saturating_add(bioseq::common::saturating_mul(distinct, 2u), 1u);
    std::uint64_t cap = 2;
    while (cap < wanted) cap <<= 1;
    return static_cast<std::uint32_t>(cap);
}

}  // namespace

runtime_sketch_result runtime::run_sketch_roundtrip(
    std::span<const std::byte> packed_sequence,
    const vulkan_rank_lut& rank_lut,
    const vulkan_sketch_params& params,
    std::uint64_t max_minimizers,
    std::span<const std::uint32_t> query_gate_words,
    std::uint32_t query_gate_mask)
{
    std::lock_guard<std::mutex> guard(mu_);
    runtime_sketch_result result;
    if (!features_.available) {
        throw vulkan_runtime_error("run_sketch_roundtrip on unavailable device");
    }
    gpu_arena& arena = reusable_gpu_arena();
    // A completed minimizer roundtrip returns only a standalone output buffer;
    // no result aliases the shared arena.  Drop any idle high-water inherited
    // from an earlier request, then cover every exit/exception path in this
    // request with the same physical-trim invariant.
    if (!arena.release_idle_capacity()) {
        throw vulkan_runtime_error(
            "gpu sketch entered with live arena slices");
    }
    gpu_arena_trim_guard arena_trim{arena};
    const std::uint32_t bp = params.bp_size;
    const std::uint32_t w = params.window_size;
    const std::uint32_t k = params.kmer_size;
    if (bp == 0u || w == 0u || k == 0u || bp < k) {
        result.count = 0;
        if (!arena_trim.release_now()) {
            throw vulkan_runtime_error(
                "gpu sketch empty roundtrip left live arena slices");
        }
        result.arena_trimmed_before_return = true;
        return result;
    }
    if (query_gate_words.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw vulkan_runtime_error("gpu sketch query gate exceeds uint32 word count");
    }
    const std::uint32_t query_gate_word_count =
        static_cast<std::uint32_t>(query_gate_words.size());
    if ((query_gate_word_count == 0u && query_gate_mask != 0u)
        || (query_gate_word_count != 0u
            && (query_gate_word_count != k_gpu_query_gate_word_count
                || query_gate_mask != k_gpu_query_gate_mask))) {
        throw vulkan_runtime_error(
            "gpu sketch query gate has truncated/noncanonical coverage");
    }
    const std::uint64_t query_gate_bytes =
        static_cast<std::uint64_t>(query_gate_words.size()) * sizeof(std::uint32_t);
    const std::uint64_t out_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(
            params.minimizer_word_kind,
            params.minimizer_words32,
            max_minimizers),
        4u);
    const gpu_sketch_memory_plan memory_plan = make_gpu_sketch_memory_plan(
        bp,
        packed_sequence.size(),
        sizeof(vulkan_rank_lut),
        max_minimizers,
        out_bytes,
        gpu_sketch_purpose::minimizer_words,
        k,
        w,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    if (!memory_plan.is_valid) {
        throw vulkan_runtime_error(fmt::format(
            "gpu sketch memory plan invalid (bp={}, max_min={}, lpu={})",
            bp,
            max_minimizers,
            params.letters_per_unit));
    }
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    // : the sketch kernel grid-strides one invocation per tile, so the
    // dispatch is ceil(num_tiles / local) workgroups CLAMPED to the device limit
    // (the shader loop covers any tiles beyond the clamped grid). No throw — a
    // window larger than 64 * max_workgroup_count_x tiles is still processed
    // correctly, just over more grid-stride steps.
    const std::uint32_t sketch_groups = std::min(
        bioseq::common::ceil_div(num_tiles, k_sketch_local_size),
        features_.max_workgroup_count_x);
    const std::uint32_t overlap = sketch_replay_overlap_bp(k, w);
    std::vector<sketch_tile_type> tiles(num_tiles);
    for (std::uint32_t t = 0; t < num_tiles; ++t) {
        const std::uint32_t lo = t * tile_bp;
        const std::uint32_t hi = std::min(bp, (t + 1u) * tile_bp);
        const std::uint32_t rlo = lo > overlap ? lo - overlap : 0u;
        tiles[t] = sketch_tile_type{params.seq_word_offset, bp, rlo, lo, hi, 0u};
    }
    const std::uint32_t num_blocks = memory_plan.tiles.scan_block_count;
    buffer out_words = create_device_buffer(out_bytes, k_compute_buffer_usage);
    if (out_words.mapped == nullptr) throw vulkan_runtime_error("gpu sketch non-unified memory");

    const std::uint64_t arena_bytes = bioseq::common::saturating_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : bioseq::common::saturating_add(query_gate_bytes, std::uint64_t{k_gpu_buffer_offset_alignment}));
    if (!arena.ensure_capacity(arena_bytes, 0, 0)) {
        throw vulkan_runtime_error(fmt::format(
            "gpu arena grow failed (bp={}, arena_bytes={})",
            bp,
            arena_bytes));
    }
    const gpu_arena_mark mark = arena.mark();

    const gpu_arena_slice seq_s =
        arena.allocate_device(memory_plan.sequence_bytes, 16u);
    const gpu_arena_slice lut_s = arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const gpu_arena_slice tiles_s = arena.allocate_device(memory_plan.tile_bytes, 16u);
    const gpu_arena_slice counts_s =
        arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const gpu_arena_slice offsets_s =
        arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const gpu_arena_slice blocks_s =
        arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const gpu_arena_slice counters_s =
        arena.allocate_device(memory_plan.counter_bytes, 16u);
    const gpu_arena_slice pairvals_s =
        arena.allocate_device(memory_plan.arena_pair_value_bytes, 16u);
    const gpu_arena_slice query_gate_s = query_gate_bytes == 0u
        ? gpu_arena_slice{}
        : arena.allocate_device(query_gate_bytes, 16u);
    if (!seq_s.is_valid() || !lut_s.is_valid() || !tiles_s.is_valid() ||
        !counts_s.is_valid() || !offsets_s.is_valid() || !blocks_s.is_valid() ||
        !counters_s.is_valid() || !pairvals_s.is_valid() ||
        (query_gate_bytes != 0u && !query_gate_s.is_valid())) {
      arena.rewind(mark);
      throw vulkan_runtime_error(fmt::format(
          "gpu sketch arena oversub (bp={} arena_bytes={} num_tiles={})",
          bp, arena_bytes, num_tiles));
    }

    auto *base = static_cast<std::byte *>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) {
#ifdef BIOSEQ_TEST_INDEXES
      note_vulkan_submission_test_upload();
#endif
      std::memcpy(base + seq_s.offset, packed_sequence.data(),
                  packed_sequence.size());
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(),
                tiles.size() * sizeof(sketch_tile_type));
    std::memset(base + counts_s.offset, 0,
                static_cast<std::size_t>(counts_s.size));
    std::memset(base + counters_s.offset, 0, 8);
    if (query_gate_bytes != 0u) {
      std::memcpy(base + query_gate_s.offset, query_gate_words.data(),
                  static_cast<std::size_t>(query_gate_bytes));
    }

    if (params.minimizer_word_kind == 0u)
      std::memset(out_words.mapped, 0,
                  static_cast<std::size_t>(out_words.bytes));

    const compute_pipeline sketch_pipe = get_compute_pipeline_locked(
        shader_kind::ordered_sketch_tile, k_sketch_local_size,
        std::max<std::uint32_t>(w, 1u));
    const compute_pipeline scan_block_pipe = get_compute_pipeline_locked(
        shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scan_add_pipe = get_compute_pipeline_locked(
        shader_kind::prefix_scan_add, 128u, 1u);
    reset_dispatch_descriptor_pool_locked();

    const gpu_arena_slice &gate_binding_s =
        query_gate_bytes == 0u ? counters_s : query_gate_s;
    const std::array<VkDescriptorBufferInfo, 9> sketch_infos{{
        {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
        {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
        {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
        {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
        {out_words.buffer_handle, 0, VK_WHOLE_SIZE},
        {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
        {offsets_s.buffer->buffer_handle, offsets_s.offset, offsets_s.size},
        {pairvals_s.buffer->buffer_handle, pairvals_s.offset, pairvals_s.size},
        {gate_binding_s.buffer->buffer_handle, gate_binding_s.offset,
         gate_binding_s.size},
    }};
    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
        {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
        {offsets_s.buffer->buffer_handle, offsets_s.offset, offsets_s.size},
        {blocks_s.buffer->buffer_handle, blocks_s.offset, blocks_s.size},
    }};

    sketch_push_constants push{0u,
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
                     num_tiles,
                     0u,
                     query_gate_mask,
                     query_gate_word_count};
    scan_push_constants scan0{num_tiles, k_prefix_scan_block_size, num_blocks, 0u};
    scan_push_constants scan1{num_tiles, k_prefix_scan_block_size, num_blocks, 1u};

    static const auto *_qsketch_d =
        bioseq::instrumentation::register_static_region("query_sketch");
    auto _qsketch_s = bioseq::instrumentation::region(_qsketch_d);
    check_vk(vkResetCommandBuffer(command_buffer_, 0),
             "vkResetCommandBuffer(pc_sketch)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin),
             "vkBeginCommandBuffer(pc_sketch)");
    record_host_to_shader(command_buffer_);
    record_compute_dispatch_locked(command_buffer_, sketch_pipe, sketch_infos,
                                   &push, sizeof(push), sketch_groups);
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(command_buffer_, scan_block_pipe, scan_infos,
                                   &scan0, sizeof(scan0), num_blocks);
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(command_buffer_, scan_add_pipe, scan_infos,
                                   &scan0, sizeof(scan0), 1u);
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(command_buffer_, scan_add_pipe, scan_infos,
                                   &scan1, sizeof(scan1),
                                   bioseq::common::ceil_div(num_tiles, 128u));
    record_compute_barrier(command_buffer_);
    push.emit_mode = 1u;
    record_compute_dispatch_locked(command_buffer_, sketch_pipe, sketch_infos,
                                   &push, sizeof(push), sketch_groups);
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_),
             "vkEndCommandBuffer(pc_sketch)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_),
             "vkQueueSubmit(pc_sketch)");
#ifdef BIOSEQ_TEST_INDEXES
    const vulkan_submission_test_fault submission_test_fault =
        delay_vulkan_submission_test_after_submit();
#endif
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX),
             "vkWaitForFences(pc_sketch)");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(pc_sketch)");
    require_immutable_metal_policy_live("sketch operation completion");
#ifdef BIOSEQ_TEST_INDEXES
    delay_vulkan_submission_test_after_drain(submission_test_fault);
    deliver_vulkan_submission_test_fault_after_drain(submission_test_fault);
#endif

    std::uint32_t last_off = 0, last_cnt = 0;
    std::memcpy(&last_off,
                base + offsets_s.offset + std::uint64_t{num_tiles - 1u} * 4u,
                4);
    std::memcpy(&last_cnt,
                base + counts_s.offset + std::uint64_t{num_tiles - 1u} * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(last_off) + last_cnt;

    if (total > max_minimizers) {
        result.overflow = true;
    } else {
        result.count = total;
        result.minimizer_words = std::move(out_words);
    }
    arena.rewind(mark);
    if (!arena_trim.release_now()) {
        result.minimizer_words.reset();
        throw vulkan_runtime_error(
            "gpu sketch completed with live arena slices");
    }
    result.arena_trimmed_before_return = true;
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(pc_sketch-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

runtime_sketch_batch_result runtime::run_sketch_tile_batch_roundtrip(
    std::span<const std::byte> packed_sequence,
    const vulkan_rank_lut& rank_lut,
    const vulkan_sketch_params& params,
    std::span<const gpu_sketch_tile_wire> tiles,
    std::span<const gpu_sketch_query_layout_wire> query_layouts,
    std::uint64_t max_minimizers,
    vulkan_sketch_host_materialization materialization)
{
    std::lock_guard<std::mutex> guard(mu_);
    runtime_sketch_batch_result result;
    if (!features_.available) {
        throw vulkan_runtime_error(
            "run_sketch_tile_batch_roundtrip on unavailable device");
    }
    if (materialization != vulkan_sketch_host_materialization::host_and_device
        && materialization != vulkan_sketch_host_materialization::device_only) {
        throw vulkan_runtime_error(
            "gpu sketch tile batch has invalid host materialization mode");
    }
    result.dispatch_stats.materialization_ = materialization;
    const std::uint32_t w = params.window_size;
    const std::uint32_t k = params.kmer_size;
    if (tiles.empty() || query_layouts.empty()
        || w == 0u || k == 0u || max_minimizers == 0u) {
        return result;
    }
    if (tiles.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw vulkan_runtime_error("gpu sketch tile batch exceeds uint32 tile count");
    }
    if (max_minimizers
        > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw vulkan_runtime_error("gpu sketch tile batch capacity exceeds ABI");
    }
    std::uint64_t layout_tile_cursor = 0u;
    for (const gpu_sketch_query_layout_wire& layout : query_layouts) {
        if (layout.begin != layout_tile_cursor
            || layout.count
                > static_cast<std::uint64_t>(tiles.size())
                    - layout_tile_cursor) {
            throw vulkan_runtime_error(
                "gpu sketch query layout does not densely partition tiles");
        }
        layout_tile_cursor += layout.count;
    }
    if (layout_tile_cursor != tiles.size()) {
        throw vulkan_runtime_error(
            "gpu sketch query layout does not cover all tiles");
    }

    const std::uint32_t tile_count = static_cast<std::uint32_t>(tiles.size());
    const gpu_sketch_batch_arena_request arena_request =
        make_sketch_batch_arena_request(
            packed_sequence.size(),
            tile_count);
    if (!arena_request.valid) {
        throw vulkan_runtime_error(
            "gpu sketch tile batch arena request is invalid");
    }
    const std::uint32_t num_blocks = arena_request.num_blocks;
    const std::uint32_t sketch_groups = std::min(
        bioseq::common::ceil_div(tile_count, k_sketch_local_size),
        features_.max_workgroup_count_x);
    if (sketch_groups == 0u) {
        return result;
    }

    // Preserve the established host-materialized path: it reserves its
    // pessimistic output up front and records count/scan/emit in one submit.
    // Only device-only mode waits for the scan before allocating exact output.
    buffer host_materialized_out_words;
    if (materialization
        == vulkan_sketch_host_materialization::host_and_device) {
        const std::uint64_t pessimistic_output_bytes =
            std::max<std::uint64_t>(
                minimizer_device_bytes(
                    params.minimizer_word_kind,
                    params.minimizer_words32,
                    max_minimizers),
                4u);
        host_materialized_out_words = create_device_buffer(
            pessimistic_output_bytes,
            k_compute_buffer_usage);
        if (host_materialized_out_words.mapped == nullptr) {
            throw vulkan_runtime_error(
                "gpu sketch tile batch non-unified host output memory");
        }
    }

    const std::uint64_t arena_bytes = arena_request.requested_bytes;
    gpu_arena& arena = reusable_gpu_arena();
    if (!arena.ensure_capacity(arena_bytes, 0u, 0u)) {
        throw vulkan_runtime_error(fmt::format(
            "gpu sketch tile batch arena grow failed (tiles={}, arena_bytes={})",
            tile_count,
            arena_bytes));
    }
    const gpu_arena_mark mark = arena.mark();
    struct arena_rewind_guard {
        gpu_arena* arena = nullptr;
        gpu_arena_mark mark{};
        ~arena_rewind_guard() {
            if (arena != nullptr) arena->rewind(mark);
        }
    } arena_rewind_on_exit{&arena, mark};

    const gpu_arena_slice seq_s = arena.allocate_device(
        std::max<std::uint64_t>(packed_sequence.size(), 4u), 16u);
    const gpu_arena_slice lut_s =
        arena.allocate_device(sizeof(vulkan_rank_lut), 16u);
    const gpu_arena_slice tiles_s = arena.allocate_device(
        static_cast<std::uint64_t>(tiles.size()) * sizeof(gpu_sketch_tile_wire),
        16u);
    const gpu_arena_slice counts_s =
        arena.allocate_device(static_cast<std::uint64_t>(tile_count) * 4u, 16u);
    const gpu_arena_slice offsets_s =
        arena.allocate_device(static_cast<std::uint64_t>(tile_count) * 4u, 16u);
    const gpu_arena_slice blocks_s =
        arena.allocate_device(static_cast<std::uint64_t>(num_blocks) * 4u, 16u);
    const gpu_arena_slice counters_s = arena.allocate_device(8u, 16u);
    const gpu_arena_slice pairvals_s = arena.allocate_device(16u, 16u);
    if (!seq_s.is_valid() || !lut_s.is_valid() || !tiles_s.is_valid()
        || !counts_s.is_valid() || !offsets_s.is_valid() || !blocks_s.is_valid()
        || !counters_s.is_valid() || !pairvals_s.is_valid()) {
        throw vulkan_runtime_error(fmt::format(
            "gpu sketch tile batch arena oversub (tiles={} arena_bytes={})",
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
        tiles.size() * sizeof(gpu_sketch_tile_wire));
    std::memset(base + counts_s.offset, 0, static_cast<std::size_t>(counts_s.size));
    std::memset(base + offsets_s.offset, 0, static_cast<std::size_t>(offsets_s.size));
    std::memset(base + blocks_s.offset, 0, static_cast<std::size_t>(blocks_s.size));
    std::memset(base + counters_s.offset, 0, static_cast<std::size_t>(counters_s.size));
    if (materialization == vulkan_sketch_host_materialization::host_and_device
        && params.minimizer_word_kind == 0u) {
        std::memset(
            host_materialized_out_words.mapped,
            0,
            static_cast<std::size_t>(host_materialized_out_words.bytes));
    }
    std::unique_ptr<runtime_pipeline_use_collector> query_pipeline_collector;
    const bool causal_pipeline_token_required =
        materialization == vulkan_sketch_host_materialization::device_only
        && (should_use_shader_provider_locked()
            || is_runtime_metal_archive_configuration_installed());
    if (causal_pipeline_token_required) {
        query_pipeline_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::query_sketch,
            1u,
            next_operation_identity_locked());
    }
    const compute_pipeline sketch_pipe = get_compute_pipeline_locked(
        shader_kind::ordered_sketch_tile,
        k_sketch_local_size,
        std::max<std::uint32_t>(w, 1u));
    const compute_pipeline scan_block_pipe = get_compute_pipeline_locked(
        shader_kind::prefix_scan_block,
        1u,
        1u);
    const compute_pipeline scan_add_pipe = get_compute_pipeline_locked(
        shader_kind::prefix_scan_add,
        128u,
        1u);
    if (query_pipeline_collector != nullptr) {
        const auto prepare = [&query_pipeline_collector](
            const compute_pipeline& pipe) {
            query_pipeline_collector->prepare(
                make_shader_key(
                    pipe.kind, 384u, pipe.local_size, pipe.max_span),
                pipe.metal_archive_receipt);
        };
        prepare(sketch_pipe);
        prepare(scan_block_pipe);
        prepare(scan_add_pipe);
    }
    reset_dispatch_descriptor_pool_locked();

    // Device-only count mode never dereferences binding 4, so it can use the
    // tiny counters slice until the exact output exists.  Host mode binds its
    // established pessimistic output, which the same submit later emits into.
    const VkDescriptorBufferInfo initial_output_info =
        materialization == vulkan_sketch_host_materialization::host_and_device
        ? VkDescriptorBufferInfo{
            host_materialized_out_words.buffer_handle, 0, VK_WHOLE_SIZE}
        : VkDescriptorBufferInfo{
            counters_s.buffer->buffer_handle,
            counters_s.offset,
            counters_s.size};
    const std::array<VkDescriptorBufferInfo, 9> count_sketch_infos{{
        {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
        {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
        {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
        {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
        initial_output_info,
        {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
        {offsets_s.buffer->buffer_handle, offsets_s.offset, offsets_s.size},
        {pairvals_s.buffer->buffer_handle, pairvals_s.offset, pairvals_s.size},
        {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
    }};
    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
        {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
        {offsets_s.buffer->buffer_handle, offsets_s.offset, offsets_s.size},
        {blocks_s.buffer->buffer_handle, blocks_s.offset, blocks_s.size},
    }};

    sketch_push_constants push{
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
    scan_push_constants scan0{tile_count, k_prefix_scan_block_size, num_blocks, 0u};
    scan_push_constants scan1{tile_count, k_prefix_scan_block_size, num_blocks, 1u};

    static const auto* _qsketch_d =
        bioseq::instrumentation::register_static_region("query_sketch_batch");
    auto _qsketch_s = bioseq::instrumentation::region(_qsketch_d);
    runtime_sketch_dispatch_stats dispatch_stats{};
    dispatch_stats.materialization_ = materialization;
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
    record_compute_dispatch_locked(
        command_buffer_, sketch_pipe, count_sketch_infos, &push, sizeof(push),
        sketch_groups, query_pipeline_collector.get(), 0u);
    ++dispatch_stats.count_dispatches_;
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(
        command_buffer_, scan_block_pipe, scan_infos, &scan0, sizeof(scan0),
        num_blocks, query_pipeline_collector.get(), 0u);
    ++dispatch_stats.prefix_scan_dispatches_;
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(
        command_buffer_, scan_add_pipe, scan_infos, &scan0, sizeof(scan0), 1u,
        query_pipeline_collector.get(), 0u);
    ++dispatch_stats.prefix_scan_dispatches_;
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(
        command_buffer_, scan_add_pipe, scan_infos, &scan1, sizeof(scan1),
        bioseq::common::ceil_div(tile_count, 128u), query_pipeline_collector.get(), 0u);
    ++dispatch_stats.prefix_scan_dispatches_;
    ++dispatch_stats.prefix_scan_sequences_;
    if (materialization
        == vulkan_sketch_host_materialization::host_and_device) {
        record_compute_barrier(command_buffer_);
        push.emit_mode = 1u;
        record_compute_dispatch_locked(
            command_buffer_, sketch_pipe, count_sketch_infos, &push,
            sizeof(push), sketch_groups);
        ++dispatch_stats.emit_dispatches_;
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
    ++dispatch_stats.completed_roundtrips_;
    result.dispatch_stats = dispatch_stats;

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
               == vulkan_sketch_host_materialization::host_and_device) {
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
        buffer out_words = create_device_buffer(
            exact_device_bytes,
            k_compute_buffer_usage);
        if (out_words.mapped == nullptr) {
            throw vulkan_runtime_error(
                "gpu sketch tile batch non-unified exact output memory");
        }
        if (params.minimizer_word_kind == 0u) {
            std::memset(
                out_words.mapped,
                0,
                static_cast<std::size_t>(out_words.bytes));
        }

        const std::array<VkDescriptorBufferInfo, 9> emit_sketch_infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {out_words.buffer_handle, 0, VK_WHOLE_SIZE},
            {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
            {offsets_s.buffer->buffer_handle, offsets_s.offset, offsets_s.size},
            {pairvals_s.buffer->buffer_handle, pairvals_s.offset, pairvals_s.size},
            {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
        }};

        // The first queue submission is complete, so its descriptor sets may
        // be recycled.  The arena mark and all resident slices remain live.
        reset_dispatch_descriptor_pool_locked();
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
        record_compute_dispatch_locked(
            command_buffer_, sketch_pipe, emit_sketch_infos, &push,
            sizeof(push), sketch_groups, query_pipeline_collector.get(), 0u);
        ++dispatch_stats.emit_dispatches_;
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
        ++dispatch_stats.completed_roundtrips_;
        result.dispatch_stats = dispatch_stats;

        result.count = total;
        result.minimizer_words = std::move(out_words);
    }
    if (!result.overflow
        && materialization == vulkan_sketch_host_materialization::device_only
        && result.minimizer_words.is_valid()
        && dispatch_stats.is_internally_consistent()) {
        if (query_pipeline_collector != nullptr) {
            result.query_authority.pipeline_token_ =
                query_pipeline_collector->seal();
        }
        result.query_authority.identity_ =
            next_operation_identity_locked();
        result.query_authority.runtime_ = this;
        result.query_authority.buffer_ = result.minimizer_words.buffer_handle;
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
        result.query_authority.dispatch_stats_ = dispatch_stats;
        result.query_authority.device_uuid_ = features_.device_uuid;
        result.query_authority.query_records_.reserve(query_layouts.size());
        result.query_authority.query_tile_counts_.reserve(
            query_layouts.size());
        std::uint64_t query_minimizer_cursor = 0u;
        for (const gpu_sketch_query_layout_wire& layout : query_layouts) {
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
                throw vulkan_runtime_error(
                    "gpu sketch query authority partition is invalid");
            }
            result.query_authority.query_records_.push_back(
                gpu_seed_query_record_wire{
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
            || !result.query_authority.does_cover_device_output()) {
            throw vulkan_runtime_error(
                "gpu sketch query authority does not cover device output");
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

/// @brief Submit an executable command buffer and wait for the synchronous runtime
/// contract. The command buffer remains executable and may be submitted again.
void submit_executable_and_wait(
    VkDevice device,
    VkQueue queue,
    VkCommandBuffer cmd,
    VkFence fence,
    const char* what,
    runtime_pipeline_use_collector* collector = nullptr)
{
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check_vk(vkQueueSubmit(queue, 1, &submit, fence), what);
    check_vk(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), what);
    check_vk(vkResetFences(device, 1, &fence), what);
    // Restore the shared fence before any policy check can throw so the next
    // synchronous operation never inherits a signalled fence.
    require_immutable_metal_policy_live(what);
    if (collector != nullptr) collector->commit(cmd);
}

/// @brief End the currently-recording command buffer, submit it, and wait. This keeps
/// the established helper contract for GPU frontend's multi-submit operations.
void submit_and_wait(
    VkDevice device,
    VkQueue queue,
    VkCommandBuffer cmd,
    VkFence fence,
    const char* what,
    runtime_pipeline_use_collector* collector = nullptr)
{
    check_vk(vkEndCommandBuffer(cmd), what);
    submit_executable_and_wait(
        device, queue, cmd, fence, what, collector);
}

}  // namespace

void runtime::prepare_target_index_pipelines_locked(
    const vulkan_sketch_params& params)
{
    // Keep this list identical to the complete target-index route. Pipeline
    // owners and their exact metadata permits remain process-lived in runtime;
    // later getters in count, radix, mark, and bucket stages are cache hits
    // under the operation's transient allocation permit.
    (void)get_compute_pipeline_locked(
        shader_kind::ordered_sketch_tile,
        k_sketch_local_size,
        std::max<std::uint32_t>(params.window_size, 1u));
    (void)get_compute_pipeline_locked(
        shader_kind::prefix_scan_block, 1u, 1u);
    (void)get_compute_pipeline_locked(
        shader_kind::prefix_scan_add, 128u, 1u);
    (void)get_compute_pipeline_locked(
        shader_kind::radix_sort_histogram, 1u, 1u);
    (void)get_compute_pipeline_locked(
        shader_kind::radix_sort_scan, 1u, 1u);
    (void)get_compute_pipeline_locked(
        shader_kind::radix_sort_scatter, 1u, 1u);
    (void)get_compute_pipeline_locked(
        shader_kind::target_index_mark_runs, 128u, 1u);
    (void)get_compute_pipeline_locked(
        shader_kind::target_index_emit_buckets, 128u, 1u);
}

void runtime::prepare_target_index_pipelines(
    const vulkan_sketch_params& params)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error(
            "prepare_target_index_pipelines on unavailable device");
    }
    if (params.window_size == 0u) {
        throw vulkan_runtime_error(
            "prepare_target_index_pipelines with zero window");
    }
    prepare_target_index_pipelines_locked(params);
}

gpu_built_index runtime::build_target_index(
    std::span<const std::byte> packed_sequence,
    std::uint64_t padded_sequence_bytes,
    std::uint64_t owned_sequence_staging_bytes,
    std::function<void()> release_owned_sequence_staging,
    const vulkan_rank_lut& rank_lut,
    const vulkan_sketch_params& params,
    std::uint64_t max_target_minimizers,
    std::span<const std::uint32_t> query_gate_words,
    std::uint32_t query_gate_mask,
    gpu_index_gate_identity query_gate_identity,
    bioseq::parallelism::memory_permit* peak_permit)
{
    std::lock_guard<std::mutex> guard(mu_);
    static const auto* _run_d = bioseq::instrumentation::register_static_region("target_index_run");
    auto _run_s = bioseq::instrumentation::region(_run_d);
    gpu_built_index out;
    out.ownership.count_submit_admissions = peak_permit != nullptr ? 1u : 0u;
    if (!features_.available) throw vulkan_runtime_error("build_target_index on unavailable device");
    const std::uint32_t bp = params.bp_size;
    const std::uint32_t w = params.window_size;
    const std::uint32_t k = params.kmer_size;
    out.target_len = bp;
    out.ownership.target_minimizer_capacity_estimate =
        max_target_minimizers;
    if (bp == 0u || w == 0u || k == 0u || bp < k || max_target_minimizers == 0u) {
        out.failure_reason = gpu_index_build_failure::invalid_shape;
        return out;  // empty/invalid shape; selected GPU callers throw.
    }
    if (max_target_minimizers > std::numeric_limits<std::uint32_t>::max()) {
        out.failure_reason =
            gpu_index_build_failure::minimizer_capacity_exceeds_u32;
        return out;
    }
    if (packed_sequence.size() > padded_sequence_bytes
        || padded_sequence_bytes
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        out.failure_reason = gpu_index_build_failure::memory_plan_invalid;
        return out;
    }
    if (query_gate_words.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw vulkan_runtime_error("gpu index query gate exceeds uint32 word count");
    }
    const std::uint32_t query_gate_word_count =
        static_cast<std::uint32_t>(query_gate_words.size());
    const bool gate_uses_identity_owner = query_gate_identity.canonical_words
        && query_gate_words.data()
            == query_gate_identity.canonical_words->data()
        && query_gate_words.size()
            == query_gate_identity.canonical_words->size();
    const bool gate_shape_matches = query_gate_word_count == 0u
        ? query_gate_identity.is_well_formed() && !query_gate_identity.enabled
        : query_gate_identity.is_well_formed()
            && query_gate_identity.enabled
            && query_gate_identity.word_count == query_gate_word_count
            && query_gate_identity.mask == query_gate_mask
            && (gate_uses_identity_owner
                || std::equal(
                    query_gate_words.begin(), query_gate_words.end(),
                    query_gate_identity.canonical_words->begin()));
    if (!gate_shape_matches) {
        throw vulkan_runtime_error(
            "gpu index query gate lacks matching immutable identity");
    }
    // Fail-safe for direct runtime callers. The production wrapper prepares
    // this same set before reserving target-index memory, so these are normally
    // cache hits. Keeping the invariant here prevents a future caller from
    // reintroducing a cold miss under the much larger exact execution peak below.
    prepare_target_index_pipelines_locked(params);
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
        k_gpu_buffer_offset_alignment,
        features_.min_storage_buffer_offset_alignment);

    const gpu_sketch_memory_plan memory_plan = make_gpu_sketch_memory_plan(
        bp,
        padded_sequence_bytes,
        sizeof(vulkan_rank_lut),
        max_target_minimizers,
        0u,
        gpu_sketch_purpose::target_index_build,
        k,
        w,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    if (!memory_plan.is_valid) {
        out.failure_reason = gpu_index_build_failure::memory_plan_invalid;
        return out;
    }
    const gpu_target_index_host_scratch_plan host_scratch_plan =
        make_gpu_target_index_host_scratch_plan(
            memory_plan, owned_sequence_staging_bytes);
    if (!host_scratch_plan.is_valid
        || (owned_sequence_staging_bytes != 0u
            && !release_owned_sequence_staging)) {
        out.failure_reason = gpu_index_build_failure::memory_plan_invalid;
        return out;
    }
    out.ownership.count_submit_host_transient_bytes =
        host_scratch_plan.count_submit_bytes;
    out.ownership.execution_submit_host_transient_bytes =
        host_scratch_plan.execution_submit_bytes;
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    // : grid-strided sketch — clamp the dispatch to the device limit.
    const std::uint32_t sketch_groups = std::min(
        bioseq::common::ceil_div(num_tiles, k_sketch_local_size),
        features_.max_workgroup_count_x);
    const std::uint32_t overlap = sketch_replay_overlap_bp(k, w);
    // Small per-call scaffolding lives in the shared arena (seq + lut + tiles +
    // tile counts/offsets). The large minimizer/sort/index arrays are sized from
    // the EXACT minimizer count (downloaded after the count pass) as standalone
    // device buffers — never a single 14 GB arena buffer that Metal cannot
    // allocate for the XXL_HIFI 500 Mbp target.
    gpu_arena& arena = reusable_gpu_arena();
    out.ownership.idle_arena_trim_attempts_before_count = 1u;
    const std::uint64_t idle_arena_bytes = bioseq::common::saturating_add(
        arena.device_buffer().allocation_bytes,
        bioseq::common::saturating_add(arena.upload_buffer().allocation_bytes,
                arena.download_buffer().allocation_bytes));
    const std::uint64_t pretrim_observed_peak = bioseq::common::saturating_add(
        idle_arena_bytes, host_scratch_plan.count_submit_bytes);
    out.ownership.pretrim_peak_observed_bytes = pretrim_observed_peak;
    if (!arena.release_idle_capacity()) {
        out.failure_reason = gpu_index_build_failure::index_invariant_failed;
        return out;
    }
    out.ownership.idle_arena_trim_successes_before_count = 1u;
    out.ownership.idle_arena_released_bytes_before_count = idle_arena_bytes;
    gpu_arena_trim_guard arena_trim{arena};
    const std::uint64_t arena_bytes = bioseq::common::saturating_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : bioseq::common::saturating_add(query_gate_bytes, std::uint64_t{k_gpu_buffer_offset_alignment}));
    const std::uint64_t count_arena_requirement = std::max(
        arena.device_buffer().allocation_bytes,
        buffer_memory_requirement_locked(arena_bytes, k_gpu_arena_usage));
    const std::uint64_t count_required_peak = bioseq::common::saturating_add(
        count_arena_requirement,
        host_scratch_plan.count_submit_bytes);
    if (peak_permit == nullptr) {
        out.failure_reason =
            gpu_index_build_failure::peak_allocation_exceeds_admission;
        return out;
    }
    if (count_required_peak > peak_permit->bytes()
        && !peak_permit->grow_to(
            count_required_peak, &out.execution_peak_admission_failure)) {
        out.failure_reason = gpu_index_build_failure::
            count_peak_memory_admission_denied;
        return out;
    }
    if (!arena.ensure_capacity(arena_bytes, 0, 0)) {
        throw vulkan_runtime_error(fmt::format(
            "gpu index arena grow failed (bp={}, arena_bytes={})",
            bp,
            arena_bytes));
    }
    const gpu_arena_mark mark = arena.mark();
    const gpu_arena_slice seq_s =
        arena.allocate_device(memory_plan.sequence_bytes, 16u);
    const gpu_arena_slice lut_s =
        arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const gpu_arena_slice tiles_s =
        arena.allocate_device(memory_plan.tile_bytes, 16u);
    const gpu_arena_slice tcounts_s =
        arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const gpu_arena_slice toffsets_s =
        arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const gpu_arena_slice tblocks_s =
        arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const gpu_arena_slice counters_s =
        arena.allocate_device(memory_plan.counter_bytes, 16u);
    const gpu_arena_slice query_gate_s = query_gate_bytes == 0u
        ? gpu_arena_slice{}
        : arena.allocate_device(query_gate_bytes, 16u);
    if (!seq_s.is_valid() || !lut_s.is_valid() || !tiles_s.is_valid() || !tcounts_s.is_valid()
        || !toffsets_s.is_valid() || !tblocks_s.is_valid() || !counters_s.is_valid()
        || (query_gate_bytes != 0u && !query_gate_s.is_valid())) {
        bioseq_log<logging_level::error>("[gpu index] scaffold arena oversubscribed (bp={}, num_tiles={}, "
                        "arena_bytes={}); returning invalid GPU index",
                        bp, num_tiles, arena_bytes);
        out.failure_reason =
            gpu_index_build_failure::device_arena_allocation_failed;
        arena.rewind(mark);
        return out;
    }

    const std::uint64_t count_observed_peak = std::max(
        pretrim_observed_peak,
        bioseq::common::saturating_add(
            arena.device_buffer().allocation_bytes,
            host_scratch_plan.count_submit_bytes));
    if (count_observed_peak > peak_permit->bytes()) {
        out.failure_reason =
            gpu_index_build_failure::peak_allocation_exceeds_admission;
        out.observed_device_peak_bytes =
            arena.device_buffer().allocation_bytes;
        arena.rewind(mark);
        return out;
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) {
        std::memcpy(
            base + seq_s.offset,
            packed_sequence.data(),
            packed_sequence.size());
    }
    const std::uint64_t sequence_padding_bytes =
        seq_s.size - static_cast<std::uint64_t>(packed_sequence.size());
    if (sequence_padding_bytes != 0u) {
        std::memset(
            base + seq_s.offset + packed_sequence.size(),
            0,
            static_cast<std::size_t>(sequence_padding_bytes));
    }
    out.ownership.direct_sequence_arena_copies = 1u;
    out.ownership.sequence_padding_zeroed_bytes = sequence_padding_bytes;
    if (release_owned_sequence_staging) {
        release_owned_sequence_staging();
        release_owned_sequence_staging = {};
        out.ownership.host_sequence_capacity_releases_before_execution = 1u;
        out.ownership.host_sequence_capacity_released_bytes =
            owned_sequence_staging_bytes;
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    for (std::uint32_t tile_index = 0u;
         tile_index < num_tiles;
         ++tile_index) {
        const std::uint32_t lo = tile_index * tile_bp;
        const std::uint32_t hi = std::min(bp, (tile_index + 1u) * tile_bp);
        const sketch_tile_type tile{
            params.seq_word_offset,
            bp,
            lo > overlap ? lo - overlap : 0u,
            lo,
            hi,
            0u};
        std::memcpy(
            base + tiles_s.offset
                + static_cast<std::uint64_t>(tile_index) * sizeof(sketch_tile_type),
            &tile,
            sizeof(tile));
    }
    out.ownership.direct_tile_arena_fills = 1u;
    out.ownership.host_tile_staging_elided_bytes =
        host_scratch_plan.elided_tile_staging_bytes;
    out.ownership.host_transient_released_before_execution_bytes =
        host_scratch_plan.released_before_execution_bytes;
    std::memset(base + tcounts_s.offset, 0, static_cast<std::size_t>(tcounts_s.size));
    if (query_gate_bytes != 0u) {
        std::memcpy(base + query_gate_s.offset, query_gate_words.data(),
                    static_cast<std::size_t>(query_gate_bytes));
    }

    const compute_pipeline sketch_pipe = get_compute_pipeline_locked(
        shader_kind::ordered_sketch_tile,
        k_sketch_local_size,
        std::max<std::uint32_t>(w, 1u));
    const compute_pipeline scan_block_pipe = get_compute_pipeline_locked(
        shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scan_add_pipe = get_compute_pipeline_locked(
        shader_kind::prefix_scan_add, 128u, 1u);

    // Count every target build on device first.  Query-gated segment indexes
    // usually retain only a tiny subset, while the ungated 500-Mbp row emits
    // 93,308,896 minimizers from a pessimistic capacity of 150,000,192.  Sizing
    // all four emit/sort streams from that pessimistic capacity made the main
    // peak C*40 bytes (6,000,007,680 bytes) even though the adopted resident
    // index is only exact-T/exact-D.  The count fence makes T authoritative
    // before any large standalone buffer exists.
    std::uint64_t emit_capacity = 0u;
    const gpu_arena_slice& gate_binding_s =
        query_gate_bytes == 0u ? counters_s : query_gate_s;
    {
        {
            const std::array<std::uint32_t, 4> cinit{0u, 0u, 0u, 0u};
            std::memcpy(base + counters_s.offset, cinit.data(), sizeof(cinit));
        }
        {
            static const auto* _rd =
                bioseq::instrumentation::register_static_region("target_sketch_count");
            auto _rs = bioseq::instrumentation::region(_rd);
            reset_dispatch_descriptor_pool_locked();
            const std::array<VkDescriptorBufferInfo, 9> sketch_infos{{
                {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
                {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
                {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
                {tcounts_s.buffer->buffer_handle, tcounts_s.offset, tcounts_s.size},
                {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
                {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
                {toffsets_s.buffer->buffer_handle, toffsets_s.offset, toffsets_s.size},
                {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
                {gate_binding_s.buffer->buffer_handle, gate_binding_s.offset, gate_binding_s.size},
            }};
            const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                {tcounts_s.buffer->buffer_handle, tcounts_s.offset, tcounts_s.size},
                {toffsets_s.buffer->buffer_handle, toffsets_s.offset, toffsets_s.size},
                {tblocks_s.buffer->buffer_handle, tblocks_s.offset, tblocks_s.size},
            }};
            sketch_push_constants push{0u, w, k, params.hash_bits, params.hash_shift, params.pos_bits,
                params.minimizer_word_kind, params.minimizer_words32, k, params.alphabet_kind,
                params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
                num_tiles, 1u, query_gate_mask, query_gate_word_count};
            const std::uint32_t num_blocks =
                memory_plan.tiles.scan_block_count;
            scan_push_constants scan0{
                num_tiles, k_prefix_scan_block_size, num_blocks, 0u};
            scan_push_constants scan1{
                num_tiles, k_prefix_scan_block_size, num_blocks, 1u};
            const std::uint32_t count_scan_block_groups =
                grid_stride_dispatch_group_count(
                    num_blocks, 1u, features_.max_workgroup_count_x);
            const std::uint32_t count_scan_add_groups =
                grid_stride_dispatch_group_count(
                    num_tiles, 128u, features_.max_workgroup_count_x);
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_count)");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_count)");
            record_host_to_shader(command_buffer_);
            record_compute_dispatch_locked(
                command_buffer_, sketch_pipe, sketch_infos, &push,
                sizeof(push), sketch_groups);
            record_compute_barrier(command_buffer_);
            record_compute_dispatch_locked(
                command_buffer_, scan_block_pipe, scan_infos, &scan0,
                sizeof(scan0), count_scan_block_groups);
            record_compute_barrier(command_buffer_);
            record_compute_dispatch_locked(
                command_buffer_, scan_add_pipe, scan_infos, &scan0,
                sizeof(scan0), 1u);
            record_compute_barrier(command_buffer_);
            record_compute_dispatch_locked(
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
            target_minimizer_count_upper_bound(bp, k);
        if ((last_count & 0x80000000u) != 0u
            || exact_count > hard_bound
            || exact_count > static_cast<std::uint64_t>(bp)
            || exact_count
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            bioseq_log<logging_level::error>(
                "[gpu idx] exact target count={} violates hard bound={} "
                "(capacity estimate={})",
                exact_count, hard_bound, max_target_minimizers);
            out.failure_reason =
                gpu_index_build_failure::output_capacity_overflow;
            arena.rewind(mark);
            return out;
        }
        out.ownership.counted_minimizer_count = exact_count;
        out.ownership.capacity_estimate_underestimated =
            exact_count > max_target_minimizers ? 1u : 0u;
        if (exact_count == 0u) {
            const std::uint64_t empty_slot_requirement =
                buffer_memory_requirement_locked(16u, k_compute_buffer_usage);
            const std::uint64_t empty_position_requirement =
                buffer_memory_requirement_locked(8u, k_compute_buffer_usage);
            const std::uint64_t empty_peak = bioseq::common::saturating_add(
                memory_plan.execution_submit_host_transient_bytes,
                bioseq::common::saturating_add(arena.device_buffer().allocation_bytes,
                    bioseq::common::saturating_add(empty_slot_requirement,
                            empty_position_requirement)));
            if (peak_permit == nullptr || !peak_permit->grow_to(
                    empty_peak, &out.execution_peak_admission_failure)) {
                out.failure_reason = gpu_index_build_failure::
                    execution_peak_memory_admission_denied;
                arena.rewind(mark);
                return out;
            }
            out.ownership.execution_peak_admissions = 1u;
            out.peak_admission_bytes = peak_permit->bytes();
            out.ownership.peak_admitted_bytes = peak_permit->bytes();
            out.slots = create_device_buffer(16u, k_compute_buffer_usage);
            out.positions = create_device_buffer(8u, k_compute_buffer_usage);
            if (out.slots.mapped == nullptr || out.positions.mapped == nullptr
                || out.slots.allocation_bytes != empty_slot_requirement
                || out.positions.allocation_bytes != empty_position_requirement) {
                out.failure_reason =
                    gpu_index_build_failure::device_buffer_allocation_failed;
                arena.rewind(mark);
                return out;
            }
            std::memset(out.slots.mapped, 0,
                        static_cast<std::size_t>(out.slots.allocation_bytes));
            std::memset(out.positions.mapped, 0,
                        static_cast<std::size_t>(out.positions.allocation_bytes));
            out.observed_device_peak_bytes = bioseq::common::saturating_add(
                arena.device_buffer().allocation_bytes,
                bioseq::common::saturating_add(out.slots.allocation_bytes,
                        out.positions.allocation_bytes));
            out.ownership.peak_observed_bytes = std::max(
                count_observed_peak,
                bioseq::common::saturating_add(
                    out.observed_device_peak_bytes,
                    memory_plan.execution_submit_host_transient_bytes));
            out.occupied_slots = 0u;
            out.position_count = 0u;
            out.slot_mask = 0u;
            check_vk(
                vkResetCommandBuffer(command_buffer_, 0),
                "reset(pc_idx_empty-post)");
            arena.rewind(mark);
            const bool arena_released = arena_trim.release_now();
            const bool arena_invalid = !arena.device_buffer().is_valid()
                && !arena.upload_buffer().is_valid()
                && !arena.download_buffer().is_valid();
            if (!arena_released || !arena_invalid) {
                out.positions.reset();
                out.slots.reset();
                out.failure_reason =
                    gpu_index_build_failure::index_invariant_failed;
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
    const std::uint64_t emit_scanned_offset =
        bioseq::common::checked_align_up(
            emit_flag_bytes, flag_scan_alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    const std::uint64_t emit_flag_scan_bytes = bioseq::common::saturating_add(
        emit_scanned_offset, emit_flag_bytes);
    const gpu_sketch_memory_plan exact_main_plan = make_gpu_sketch_memory_plan(
        bp,
        padded_sequence_bytes,
        sizeof(vulkan_rank_lut),
        emit_capacity,
        0u,
        gpu_sketch_purpose::target_index_build,
        k,
        w,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    const auto allocation_requirement = [this](std::uint64_t bytes) {
        return buffer_memory_requirement_locked(bytes, k_compute_buffer_usage);
    };
    const std::uint64_t initial_key_requirement = allocation_requirement(
        std::max<std::uint64_t>(emit_capacity * 8u, emit_flag_scan_bytes));
    const std::uint64_t initial_value_requirement =
        allocation_requirement(emit_capacity * 8u);
    const std::uint64_t exact_flag_bytes = emit_capacity * 4u;
    const std::uint64_t exact_scanned_offset =
        bioseq::common::checked_align_up(
            exact_flag_bytes, flag_scan_alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    const std::uint64_t exact_flag_scan_bytes = bioseq::common::saturating_add(
        exact_scanned_offset, exact_flag_bytes);
    const std::uint64_t scratch_key_requirement = allocation_requirement(
        std::max<std::uint64_t>(emit_capacity * 8u, exact_flag_scan_bytes));
    const std::uint64_t scratch_value_requirement =
        allocation_requirement(emit_capacity * 8u);
    const workgroup_plan peak_wg = plan_workgroups(
        emit_capacity, features_.max_workgroup_count_x);
    const std::uint64_t histogram_requirement = allocation_requirement(
        std::uint64_t{peak_wg.num_workgroups} * 256u * 4u);
    const std::uint64_t scan_block_requirement = allocation_requirement(
        std::uint64_t{bioseq::common::ceil_div(emit_capacity, k_prefix_scan_block_size)} * 4u);
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
    const std::uint64_t sort_device_peak = bioseq::common::saturating_add(
        arena_requirement,
        bioseq::common::saturating_add(initial_key_requirement,
            bioseq::common::saturating_add(initial_value_requirement,
                bioseq::common::saturating_add(scratch_key_requirement,
                    bioseq::common::saturating_add(scratch_value_requirement,
                            histogram_requirement)))));
    const std::uint64_t mark_device_peak = bioseq::common::saturating_add(
        arena_requirement,
        bioseq::common::saturating_add(sorted_key_requirement,
            bioseq::common::saturating_add(scratch_value_requirement,
                bioseq::common::saturating_add(flag_workspace_requirement,
                        scan_block_requirement))));
    const std::uint64_t bucket_device_peak = bioseq::common::saturating_add(
        arena_requirement,
        bioseq::common::saturating_add(sorted_key_requirement,
            bioseq::common::saturating_add(scratch_value_requirement,
                bioseq::common::saturating_add(flag_workspace_requirement,
                        bucket_requirement))));
    const std::uint64_t exact_device_peak = std::max(
        sort_device_peak, std::max(mark_device_peak, bucket_device_peak));
    const std::uint64_t exact_execution_admission = bioseq::common::saturating_add(
        exact_device_peak,
        exact_main_plan.execution_submit_host_transient_bytes);
    if (peak_permit == nullptr || !exact_main_plan.is_valid
        || exact_execution_admission == (std::numeric_limits<std::uint64_t>::max)()) {
        out.failure_reason =
            gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    if (!peak_permit->grow_to(
            exact_execution_admission, &out.execution_peak_admission_failure)) {
        out.failure_reason =
            gpu_index_build_failure::execution_peak_memory_admission_denied;
        arena.rewind(mark);
        return out;
    }
    out.ownership.execution_peak_admissions = 1u;
    out.peak_admission_bytes = peak_permit->bytes();
    out.ownership.peak_admitted_bytes = peak_permit->bytes();

    buffer keys = create_device_buffer(
        std::max<std::uint64_t>(emit_capacity * 8u, emit_flag_scan_bytes),
        k_compute_buffer_usage);   // out_words: u64 hash/min; later flags+scan alias
    buffer vals = create_device_buffer(emit_capacity * 8u, k_compute_buffer_usage);   // out_pair_vals: u64 pos_strand/min
    if (keys.mapped == nullptr || vals.mapped == nullptr
        || keys.allocation_bytes != initial_key_requirement
        || vals.allocation_bytes != initial_value_requirement) {
        bioseq_log<logging_level::error>("[gpu index] capacity buffer alloc failed (cap={}); "
                        "returning invalid GPU index", emit_capacity);
        out.failure_reason =
            gpu_index_build_failure::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    const std::uint64_t emit_observed_device = bioseq::common::saturating_add(
        arena.device_buffer().allocation_bytes,
        bioseq::common::saturating_add(keys.allocation_bytes, vals.allocation_bytes));
    out.observed_device_peak_bytes = std::max(
        out.observed_device_peak_bytes, emit_observed_device);
    if (bioseq::common::saturating_add(emit_observed_device,
                exact_main_plan.execution_submit_host_transient_bytes)
            > peak_permit->bytes()) {
        out.failure_reason =
            gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const gpu_arena_slice keys_s{&keys, 0, keys.bytes};
    const gpu_arena_slice vals_s{&vals, 0, vals.bytes};
    {
        const std::array<std::uint32_t, 4> cinit{
            0u, static_cast<std::uint32_t>(emit_capacity), 0u, 0u};
        std::memcpy(base + counters_s.offset, cinit.data(), sizeof(cinit));
    }
    {
        static const auto* _rd = bioseq::instrumentation::register_static_region("target_sketch");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
        const std::array<VkDescriptorBufferInfo, 9> infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
            {tcounts_s.buffer->buffer_handle, tcounts_s.offset, tcounts_s.size},
            {keys_s.buffer->buffer_handle, keys_s.offset, keys_s.size},             // out_words
            {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
            {toffsets_s.buffer->buffer_handle, toffsets_s.offset, toffsets_s.size},
            {vals_s.buffer->buffer_handle, vals_s.offset, vals_s.size},             // out_pair_vals
            {gate_binding_s.buffer->buffer_handle, gate_binding_s.offset, gate_binding_s.size},
        }};
        sketch_push_constants push{1u, w, k, params.hash_bits, params.hash_shift, params.pos_bits,
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
        record_compute_dispatch_locked(command_buffer_, sketch_pipe, infos, &push, sizeof(push), sketch_groups);
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
    constexpr std::uint32_t k_target_emit_complete = 0x80000000u;
    constexpr std::uint32_t k_target_emit_capacity_failure = 1u;
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
        const std::uint64_t actual = marker & ~k_target_emit_complete;
        emit_parity_valid = (marker & k_target_emit_complete) != 0u
            && offset <= next_offset
            && next_offset <= emit_capacity
            && actual == next_offset - offset;
        verified_emit_count = bioseq::common::saturating_add(verified_emit_count, actual);
    }
    emit_parity_valid = emit_parity_valid
        && verified_emit_count == emit_capacity;
    if (!emit_parity_valid) {
        bioseq_log<logging_level::error>(
            "[gpu idx] deterministic emit parity failed "
            "(status={}, counted={}, verified={})",
            emit_status, emit_capacity, verified_emit_count);
        out.failure_reason =
            (emit_status & k_target_emit_capacity_failure) != 0u
                ? gpu_index_build_failure::output_capacity_overflow
                : gpu_index_build_failure::index_invariant_failed;
        arena.rewind(mark);
        return out;
    }
    const std::uint64_t tm = emit_capacity;
    out.ownership.verified_emit_count = verified_emit_count;
    out.ownership.count_emit_parity_verified = 1u;
    out.ownership.retained_minimizer_count = tm;

    const std::uint64_t tm_flag_bytes = tm * 4u;
    const std::uint64_t tm_scanned_offset =
        bioseq::common::checked_align_up(
            tm_flag_bytes, flag_scan_alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    const std::uint64_t tm_flag_scan_bytes = bioseq::common::saturating_add(
        tm_scanned_offset, tm_flag_bytes);
    const std::uint64_t tm_scratch_key_requirement = allocation_requirement(
        std::max<std::uint64_t>(tm * 8u, tm_flag_scan_bytes));
    const std::uint64_t tm_scratch_value_requirement =
        allocation_requirement(tm * 8u);
    const workgroup_plan tm_expected_wg = plan_workgroups(
        tm, features_.max_workgroup_count_x);
    const std::uint64_t tm_histogram_requirement = allocation_requirement(
        std::uint64_t{tm_expected_wg.num_workgroups} * 256u * 4u);
    const std::uint64_t tm_scan_block_requirement = allocation_requirement(
        std::uint64_t{bioseq::common::ceil_div(tm, k_prefix_scan_block_size)} * 4u);

    // Exact-T sort scratch.  Lifetimes are split at GPU fences below so the
    // bucket stage never overlaps the histogram and the unused value buffer.
    const workgroup_plan swg = plan_workgroups(tm, features_.max_workgroup_count_x);
    const std::uint64_t flag_bytes = tm * 4u;
    const std::uint64_t scanned_offset =
        bioseq::common::checked_align_up(flag_bytes, flag_scan_alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    const std::uint64_t flag_scan_bytes = bioseq::common::saturating_add(scanned_offset, flag_bytes);
    buffer keys_scr, vals_scr, hist;
    {
        static const auto* _alloc_d = bioseq::instrumentation::register_static_region("target_index_allocation");
        auto _alloc_s = bioseq::instrumentation::region(_alloc_d);
        keys_scr = create_device_buffer(
            std::max<std::uint64_t>(tm * 8u, flag_scan_bytes),
            k_compute_buffer_usage);
        vals_scr = create_device_buffer(tm * 8u, k_compute_buffer_usage);
        hist = create_device_buffer(std::uint64_t{swg.num_workgroups} * 256u * 4u, k_compute_buffer_usage);
    }
    if (keys_scr.mapped == nullptr || vals_scr.mapped == nullptr
        || hist.mapped == nullptr
        || keys_scr.allocation_bytes != tm_scratch_key_requirement
        || vals_scr.allocation_bytes != tm_scratch_value_requirement
        || hist.allocation_bytes != tm_histogram_requirement) {
        bioseq_log<logging_level::error>("[gpu index] scratch buffer alloc failed (tm={}, num_wg={}); "
                        "returning invalid GPU index",
                        tm, swg.num_workgroups);
        out.failure_reason =
            gpu_index_build_failure::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    const gpu_arena_slice keys_scr_s{&keys_scr, 0, keys_scr.bytes};
    const gpu_arena_slice vals_scr_s{&vals_scr, 0, vals_scr.bytes};
    const gpu_arena_slice hist_s{&hist, 0, hist.bytes};

    out.observed_device_peak_bytes = bioseq::common::saturating_add(
        arena.device_buffer().allocation_bytes,
        bioseq::common::saturating_add(keys.allocation_bytes,
            bioseq::common::saturating_add(vals.allocation_bytes,
                bioseq::common::saturating_add(keys_scr.allocation_bytes,
                    bioseq::common::saturating_add(vals_scr.allocation_bytes,
                            hist.allocation_bytes)))));
    if (bioseq::common::saturating_add(out.observed_device_peak_bytes,
                exact_main_plan.execution_submit_host_transient_bytes)
            > peak_permit->bytes()) {
        out.failure_reason =
            gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }

    // --- Submit 2a: radix sort pairs by hash. ---
    // Only sort the significant hash bytes: for K=15 dna4 the stored hash is
    // ~30 bits → 4 passes, not 8, roughly halving the sort.
    const std::uint32_t hash_passes = std::clamp<std::uint32_t>(
        (params.hash_bits + 7u) / 8u, 1u, 8u);
    gpu_sort_plan sort_plan{tm, 8u, 8u, 8u, hash_passes};
    bool sorted_in_scratch = false;
    {
        static const auto* _rd = bioseq::instrumentation::register_static_region("target_index_sort");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
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
                command_buffer_, vals.buffer_handle, vals_scr.buffer_handle, 1u, &copy);
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
    buffer positions_exact;
    buffer flag_scan;
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
            gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const gpu_arena_slice sorted_keys_s = sorted_in_scratch
        ? gpu_arena_slice{&keys_scr, 0u, tm * 8u}
        : gpu_arena_slice{&keys, 0u, tm * 8u};
    const gpu_arena_slice flags_s{&flag_scan, 0u, flag_bytes};
    const gpu_arena_slice scanned_s{
        &flag_scan, scanned_offset, flag_bytes};
    buffer iblocks = create_device_buffer(
        std::uint64_t{bioseq::common::ceil_div(tm, k_prefix_scan_block_size)} * 4u,
        k_compute_buffer_usage);
    if (iblocks.mapped == nullptr) {
        out.failure_reason =
            gpu_index_build_failure::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    if (iblocks.allocation_bytes != tm_scan_block_requirement) {
        out.failure_reason =
            gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const gpu_arena_slice iblocks_s{&iblocks, 0u, iblocks.bytes};
    out.observed_device_peak_bytes = std::max(
        out.observed_device_peak_bytes,
        bioseq::common::saturating_add(arena.device_buffer().allocation_bytes,
            bioseq::common::saturating_add(sorted_keys_s.buffer->allocation_bytes,
                bioseq::common::saturating_add(positions_exact.allocation_bytes,
                    bioseq::common::saturating_add(flag_scan.allocation_bytes,
                            iblocks.allocation_bytes)))));
    if (bioseq::common::saturating_add(out.observed_device_peak_bytes,
                exact_main_plan.execution_submit_host_transient_bytes)
            > peak_permit->bytes()) {
        out.failure_reason =
            gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const VkDescriptorBufferInfo dummy{
        counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size};

    // --- Submit 2b: mark runs and scan flags after sort scratch is released. ---
    {
        static const auto* _rd =
            bioseq::instrumentation::register_static_region("target_index_mark_scan");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_mark)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_mark)");
        record_host_to_shader(command_buffer_);
        record_compute_barrier(command_buffer_);
        const compute_pipeline mark_pipe = get_compute_pipeline_locked(
            shader_kind::target_index_mark_runs, 128u, 1u);
        const std::array<VkDescriptorBufferInfo, 7> idx_infos{{
            {sorted_keys_s.buffer->buffer_handle, sorted_keys_s.offset, sorted_keys_s.size},
            {flags_s.buffer->buffer_handle, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer_handle, scanned_s.offset, scanned_s.size},
            dummy, dummy, dummy, dummy,
        }};
        index_push_constants ipush{static_cast<std::uint32_t>(tm), 0u, 0u, 0u, 0u, 0u};
        const std::uint32_t mark_groups =
            grid_stride_dispatch_group_count(
                tm, 128u, features_.max_workgroup_count_x);
        record_compute_dispatch_locked(
            command_buffer_, mark_pipe, idx_infos, &ipush, sizeof(ipush),
            mark_groups);
        record_compute_barrier(command_buffer_);
        const compute_pipeline scanb = get_compute_pipeline_locked(
            shader_kind::prefix_scan_block, 1u, 1u);
        const compute_pipeline scana = get_compute_pipeline_locked(
            shader_kind::prefix_scan_add, 128u, 1u);
        const std::uint32_t inb = bioseq::common::ceil_div(tm, k_prefix_scan_block_size);
        const std::array<VkDescriptorBufferInfo, 3> iscan{{
            {flags_s.buffer->buffer_handle, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer_handle, scanned_s.offset, scanned_s.size},
            {iblocks_s.buffer->buffer_handle, iblocks_s.offset, iblocks_s.size},
        }};
        scan_push_constants is0{static_cast<std::uint32_t>(tm), k_prefix_scan_block_size, inb, 0u};
        scan_push_constants is1{static_cast<std::uint32_t>(tm), k_prefix_scan_block_size, inb, 1u};
        const std::uint32_t scan_block_groups =
            grid_stride_dispatch_group_count(
                inb, 1u, features_.max_workgroup_count_x);
        const std::uint32_t scan_add_groups =
            grid_stride_dispatch_group_count(
                tm, 128u, features_.max_workgroup_count_x);
        record_compute_dispatch_locked(
            command_buffer_, scanb, iscan, &is0, sizeof(is0),
            scan_block_groups);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, scana, iscan, &is0, sizeof(is0), 1u);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(
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
        bioseq_log<logging_level::error>("[gpu idx] num_buckets=0 (tm={})", tm);
        out.failure_reason = gpu_index_build_failure::index_invariant_failed;
        arena.rewind(mark); return out;
    }

    // The dense bucket array is built sorted by hash (the pairs were radix-sorted
    // by hash), so the seed lookup binary-searches it directly — no open-addressed
    // slot table. Building a 134 M-slot table with 50 M atomicCompSwap probes
    // tripped the macOS GPU watchdog (VK_ERROR_DEVICE_LOST) at XXL_HIFI; the
    // sorted-bucket form removes that whole submit. `buckets` becomes the resident
    // index ("slots"); `positions` is the hash-run-ordered pos_strand array.
    iblocks.reset();
    buffer buckets = create_device_buffer(
        std::uint64_t{num_buckets} * 16u, k_compute_buffer_usage);
    if (buckets.mapped == nullptr) {
        out.failure_reason =
            gpu_index_build_failure::device_buffer_allocation_failed;
        arena.rewind(mark);
        return out;
    }
    const std::uint64_t exact_bucket_requirement =
        allocation_requirement(std::uint64_t{num_buckets} * 16u);
    if (buckets.allocation_bytes != exact_bucket_requirement) {
        out.failure_reason =
            gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }
    const gpu_arena_slice buckets_s{&buckets, 0u, buckets.bytes};
    {
        static const auto* _bd = bioseq::instrumentation::register_static_region("target_index_bucket_memory");
        auto _bs = bioseq::instrumentation::region(_bd);
        // Touch the complete allocation, including Vk padding, so resident
        // evidence never fabricates full-touch coverage.
        std::memset(
            buckets.mapped, 0,
            static_cast<std::size_t>(buckets.allocation_bytes));
    }

    out.observed_device_peak_bytes = std::max(
        out.observed_device_peak_bytes,
        bioseq::common::saturating_add(arena.device_buffer().allocation_bytes,
            bioseq::common::saturating_add(sorted_keys_s.buffer->allocation_bytes,
                bioseq::common::saturating_add(positions_exact.allocation_bytes,
                    bioseq::common::saturating_add(flag_scan.allocation_bytes,
                            buckets.allocation_bytes)))));
    if (bioseq::common::saturating_add(out.observed_device_peak_bytes,
                exact_main_plan.execution_submit_host_transient_bytes)
            > peak_permit->bytes()) {
        out.failure_reason =
            gpu_index_build_failure::peak_allocation_exceeds_admission;
        arena.rewind(mark);
        return out;
    }

    // --- Submit 3: emit dense sorted buckets (single light dispatch). ---
    {
        static const auto* _rd = bioseq::instrumentation::register_static_region("target_index_slots");
        auto _rs = bioseq::instrumentation::region(_rd);
        const gpu_arena_slice& sorted_keys = sorted_in_scratch ? keys_scr_s : keys_s;
        const std::array<VkDescriptorBufferInfo, 7> idx_infos{{
            {sorted_keys.buffer->buffer_handle, sorted_keys.offset, sorted_keys.size},
            {flags_s.buffer->buffer_handle, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer_handle, scanned_s.offset, scanned_s.size},
            {buckets_s.buffer->buffer_handle, buckets_s.offset, buckets_s.size},
            dummy, dummy, dummy,
        }};
        index_push_constants ipush{static_cast<std::uint32_t>(tm), 0u, 0u, num_buckets, 0u, 0u};
        reset_dispatch_descriptor_pool_locked();
        const compute_pipeline emit_b = get_compute_pipeline_locked(
            shader_kind::target_index_emit_buckets, 128u, 1u);
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx3)");
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx3)");
        record_host_to_shader(command_buffer_);
        record_compute_barrier(command_buffer_);
        const std::uint32_t bucket_groups =
            grid_stride_dispatch_group_count(
                tm, 128u, features_.max_workgroup_count_x);
        record_compute_dispatch_locked(
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
    out.ownership.peak_observed_bytes = std::max(
        count_observed_peak,
        bioseq::common::saturating_add(
            out.observed_device_peak_bytes,
            exact_main_plan.execution_submit_host_transient_bytes));
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx-post)");
    arena.rewind(mark);
    const bool arena_released = arena_trim.release_now();
    const bool arena_invalid = !arena.device_buffer().is_valid()
        && !arena.upload_buffer().is_valid()
        && !arena.download_buffer().is_valid();
    if (!arena_released || !arena_invalid) {
        out.positions.reset();
        out.slots.reset();
        out.failure_reason =
            gpu_index_build_failure::index_invariant_failed;
        return out;
    }
    out.ownership.arena_trimmed_before_permit_shrink = 1u;
    out.valid = true;
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

gpu_sorted_pair_result runtime::run_sketch_sort_pairs_roundtrip(
    std::span<const std::byte> packed_sequence,
    const vulkan_rank_lut& rank_lut,
    const vulkan_sketch_params& params,
    std::uint64_t max_target_minimizers)
{
    std::lock_guard<std::mutex> guard(mu_);
    static const auto* _run_d =
        bioseq::instrumentation::register_static_region("sorted_pair_run");
    auto _run_s = bioseq::instrumentation::region(_run_d);

    gpu_sorted_pair_result out;
    if (!features_.available) {
        throw vulkan_runtime_error(
            "run_sketch_sort_pairs_roundtrip on unavailable device");
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

    const gpu_sketch_memory_plan memory_plan = make_gpu_sketch_memory_plan(
        bp,
        packed_sequence.size(),
        sizeof(vulkan_rank_lut),
        max_target_minimizers,
        0u,
        gpu_sketch_purpose::sorted_pairs,
        kmer_size,
        window_size,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    if (!memory_plan.is_valid) {
        return out;
    }
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    if (num_tiles == 0u) {
        return out;
    }
    // : grid-strided sketch — clamp the dispatch to the device limit.
    const std::uint32_t sketch_groups = std::min(
        bioseq::common::ceil_div(num_tiles, k_sketch_local_size),
        features_.max_workgroup_count_x);

    const std::uint32_t overlap =
        sketch_replay_overlap_bp(kmer_size, window_size);
    std::vector<sketch_tile_type> tiles(num_tiles);
    for (std::uint32_t tile = 0; tile < num_tiles; ++tile) {
        const std::uint32_t lo = tile * tile_bp;
        const std::uint32_t hi = std::min(bp, (tile + 1u) * tile_bp);
        tiles[tile] = sketch_tile_type{
            params.seq_word_offset,
            bp,
            lo > overlap ? lo - overlap : 0u,
            lo,
            hi,
            0u};
    }

    gpu_arena& arena = reusable_gpu_arena();
    if (!arena.ensure_capacity(memory_plan.arena_scaffold_bytes, 0u, 0u)) {
        throw vulkan_runtime_error(fmt::format(
            "gpu sorted-pair arena grow failed (bp={}, arena_bytes={})",
            bp,
            memory_plan.arena_scaffold_bytes));
    }

    const gpu_arena_mark mark = arena.mark();
    const gpu_arena_slice seq_s =
        arena.allocate_device(memory_plan.sequence_bytes, 16u);
    const gpu_arena_slice lut_s =
        arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const gpu_arena_slice tiles_s =
        arena.allocate_device(memory_plan.tile_bytes, 16u);
    const gpu_arena_slice tcounts_s =
        arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const gpu_arena_slice toffsets_s =
        arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const gpu_arena_slice tblocks_s =
        arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const gpu_arena_slice counters_s =
        arena.allocate_device(memory_plan.counter_bytes, 16u);
    if (!seq_s.is_valid() || !lut_s.is_valid() || !tiles_s.is_valid()
        || !tcounts_s.is_valid() || !toffsets_s.is_valid() || !tblocks_s.is_valid()
        || !counters_s.is_valid()) {
        arena.rewind(mark);
        return out;
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) {
        std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(sketch_tile_type));
    std::memset(base + tcounts_s.offset, 0, static_cast<std::size_t>(tcounts_s.size));

    const compute_pipeline sketch_pipe = get_compute_pipeline_locked(
        shader_kind::ordered_sketch_tile,
        k_sketch_local_size,
        std::max<std::uint32_t>(window_size, 1u));

    const std::uint64_t capacity = max_target_minimizers;
    buffer keys = create_device_buffer(capacity * 8u, k_compute_buffer_usage);
    buffer values = create_device_buffer(capacity * 8u, k_compute_buffer_usage);
    if (keys.mapped == nullptr || values.mapped == nullptr) {
        arena.rewind(mark);
        return out;
    }

    const gpu_arena_slice keys_s{&keys, 0u, keys.bytes};
    const gpu_arena_slice values_s{&values, 0u, values.bytes};
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
            bioseq::instrumentation::register_static_region("sorted_pair_sketch");
        auto _sketch_s = bioseq::instrumentation::region(_sketch_d);
        reset_dispatch_descriptor_pool_locked();
        const std::array<VkDescriptorBufferInfo, 9> infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer_handle, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer_handle, tiles_s.offset, tiles_s.size},
            {tcounts_s.buffer->buffer_handle, tcounts_s.offset, tcounts_s.size},
            {keys_s.buffer->buffer_handle, keys_s.offset, keys_s.size},
            {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
            {toffsets_s.buffer->buffer_handle, toffsets_s.offset, toffsets_s.size},
            {values_s.buffer->buffer_handle, values_s.offset, values_s.size},
            {counters_s.buffer->buffer_handle, counters_s.offset, counters_s.size},
        }};
        sketch_push_constants push{
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
        record_compute_dispatch_locked(
            command_buffer_, sketch_pipe, infos, &push, sizeof(push),
            sketch_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(
            device_, queue_, command_buffer_, fence_, "submit(pc_sorted_pair_emit)");
    }

    std::uint32_t pair_count32 = 0;
    std::memcpy(&pair_count32, base + counters_s.offset, sizeof(pair_count32));
    const std::uint64_t pair_count = pair_count32;
    // A window with zero minimizers (for example, an all-N region) is a
    // LEGITIMATE empty result, not a GPU failure: return valid + empty so the
    // caller emits no pairs rather than throwing vulkan_runtime_error. Only an
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

    const workgroup_plan sort_wg = plan_workgroups(pair_count, features_.max_workgroup_count_x);
    buffer keys_scratch = create_device_buffer(pair_count * 8u, k_compute_buffer_usage);
    buffer values_scratch = create_device_buffer(pair_count * 8u, k_compute_buffer_usage);
    buffer histogram = create_device_buffer(
        std::uint64_t{sort_wg.num_workgroups} * 256u * 4u,
        k_compute_buffer_usage);
    if (keys_scratch.mapped == nullptr || values_scratch.mapped == nullptr
        || histogram.mapped == nullptr) {
        arena.rewind(mark);
        return out;
    }

    sorted_pair_device_views views{
        .keys = keys_s,
        .values = values_s,
        .keys_scratch = gpu_arena_slice{&keys_scratch, 0u, keys_scratch.bytes},
        .values_scratch = gpu_arena_slice{&values_scratch, 0u, values_scratch.bytes},
        .keys_in_scratch = false,
    };
    const gpu_arena_slice histogram_s{&histogram, 0u, histogram.bytes};
    const gpu_sort_plan value_sort_plan{
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
    const gpu_sort_plan key_sort_plan{
        .element_count = pair_count,
        .key_bytes = 8u,
        .value_bytes = 8u,
        .radix_bits = 8u,
        .pass_count = hash_passes,
    };

    {
        static const auto* _sort_d =
            bioseq::instrumentation::register_static_region("sorted_pair_sort");
        auto _sort_s = bioseq::instrumentation::region(_sort_d);
        reset_dispatch_descriptor_pool_locked();
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

    const gpu_arena_slice& sorted_keys = choose_sorted_keys(views);
    const gpu_arena_slice& sorted_values = choose_sorted_values(views);
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

// — GPU sequence sort. Two submits (modelled on the index build):
//  1: upload seq+descs -> extract 64-bit canonical key per row -> radix sort.
//  host: build equal-key run ranges; seed `index` from the sorted row ids.
//  2: per-run bitonic tie-break over the full canonical compare; if any run is
//     oversized, continue in the same submit with global full-compare bitonic
//     stages over the padded permutation. Then download the permutation.
std::vector<std::uint32_t> runtime::run_sequence_sort_roundtrip(
    std::span<const std::byte> packed_units,
    std::span<const sequence_descriptor_wire> descs,
    const sequence_sort_kernel_parameters& params)
{
    const sequence_sort_upload_chunk chunk{
        packed_units.data(),
        static_cast<std::uint64_t>(packed_units.size()),
        static_cast<std::uint64_t>(packed_units.size())};
    return run_sequence_sort_roundtrip(std::span<const sequence_sort_upload_chunk>(&chunk, 1u),
                                       descs, params);
}

std::vector<std::uint32_t> runtime::run_sequence_sort_roundtrip(
    std::span<const sequence_sort_upload_chunk> chunks,
    std::span<const sequence_descriptor_wire> descs,
    const sequence_sort_kernel_parameters& params)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_sequence_sort_roundtrip on unavailable device");
    }
    const std::uint32_t n = static_cast<std::uint32_t>(descs.size());
    if (n == 0) return {};
    if (n == 1) return std::vector<std::uint32_t>{0u};

    std::uint64_t packed_unit_bytes = 0;
    for (const sequence_sort_upload_chunk& chunk : chunks) {
        if (chunk.byte_size > chunk.padded_byte_size) {
            throw vulkan_runtime_error("run_sequence_sort_roundtrip: invalid upload chunk padding");
        }
        if (chunk.byte_size > 0u && chunk.bytes == nullptr) {
            throw vulkan_runtime_error("run_sequence_sort_roundtrip: non-empty upload chunk has null data");
        }
        if (packed_unit_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - chunk.padded_byte_size) {
            throw vulkan_runtime_error("run_sequence_sort_roundtrip: upload chunks exceed uint64");
        }
        packed_unit_bytes += chunk.padded_byte_size;
    }
    seq_sort_uploaded_bytes_.fetch_add(
        packed_unit_bytes,
        std::memory_order_relaxed);

    static constexpr std::uint32_t k_sequence_sort_local_size = 256u;
    const bool dna4 =
        params.alphabet_kind == static_cast<std::uint32_t>(alphabet_kernel_kind::dna4_nt4);
    const shader_kind key_kind = dna4 ? shader_kind::seq_sort_extract_key_dna4
                                     : shader_kind::seq_sort_extract_key_generic;
    const shader_kind tb_kind  = dna4 ? shader_kind::seq_sort_tiebreak_dna4
                                     : shader_kind::seq_sort_tiebreak_generic;
    const shader_kind gc_kind  = dna4 ? shader_kind::seq_sort_global_compare_dna4
                                     : shader_kind::seq_sort_global_compare_generic;
    const shader_kind dense_kind = dna4 ? shader_kind::seq_sort_dense_scatter_dna4
                                       : shader_kind::seq_sort_dense_scatter_generic;
    const shader_kind prefix_kind = dna4 ? shader_kind::seq_sort_prefix_check_dna4
                                        : shader_kind::seq_sort_prefix_check_generic;
    const compute_pipeline key_pipe = get_compute_pipeline_locked(key_kind, k_sequence_sort_local_size, 0u);
    const compute_pipeline tb_pipe  = get_compute_pipeline_locked(tb_kind, k_sequence_sort_local_size, 0u);
    const compute_pipeline gc_pipe  = get_compute_pipeline_locked(gc_kind, k_sequence_sort_local_size, 0u);
    const compute_pipeline dense_pipe = get_compute_pipeline_locked(dense_kind, k_sequence_sort_local_size, 0u);
    const compute_pipeline prefix_pipe = get_compute_pipeline_locked(prefix_kind, k_sequence_sort_local_size, 0u);
    const workgroup_plan swg = plan_workgroups(n, features_.max_workgroup_count_x);
    auto next_power_of_two = [](std::uint32_t value) -> std::uint32_t {
        if (value <= 1u) return 1u;
        if (value > 0x80000000u) {
            throw vulkan_runtime_error("run_sequence_sort_roundtrip: batch too large for padded bitonic index");
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
    buffer seq        = create_device_buffer(seq_bytes, k_compute_buffer_usage);
    buffer descs_buf  = create_device_buffer(std::uint64_t{n} * 16u, k_compute_buffer_usage);
    buffer keys       = create_device_buffer(std::uint64_t{n} * 8u, k_compute_buffer_usage);
    buffer vals       = create_device_buffer(std::uint64_t{n} * 8u, k_compute_buffer_usage);
    buffer keys_scr   = create_device_buffer(std::uint64_t{n} * 8u, k_compute_buffer_usage);
    buffer vals_scr   = create_device_buffer(std::uint64_t{n} * 8u, k_compute_buffer_usage);
    buffer hist       = create_device_buffer(std::uint64_t{swg.num_workgroups} * 256u * 4u, k_compute_buffer_usage);
    buffer index_buf  = create_device_buffer(std::uint64_t{padded_n} * 4u, k_compute_buffer_usage);
    buffer dense_status = create_device_buffer(4u, k_compute_buffer_usage);
    if (seq.mapped == nullptr || descs_buf.mapped == nullptr || keys.mapped == nullptr
        || vals.mapped == nullptr || keys_scr.mapped == nullptr || vals_scr.mapped == nullptr
        || hist.mapped == nullptr || index_buf.mapped == nullptr
        || dense_status.mapped == nullptr) {
        throw vulkan_runtime_error("run_sequence_sort_roundtrip: device buffer alloc failed");
    }
    auto* seq_base = static_cast<std::byte*>(seq.mapped);
    std::uint64_t write_offset = 0;
    for (const sequence_sort_upload_chunk& chunk : chunks) {
        if (chunk.byte_size >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            throw vulkan_runtime_error("run_sequence_sort_roundtrip: upload chunk exceeds size_t");
        }
        if (chunk.padded_byte_size - chunk.byte_size >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            throw vulkan_runtime_error("run_sequence_sort_roundtrip: upload padding exceeds size_t");
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
    std::memcpy(descs_buf.mapped, descs.data(), descs.size() * sizeof(sequence_descriptor_wire));

    const gpu_arena_slice seq_s{&seq, 0, seq.bytes};
    const gpu_arena_slice descs_s{&descs_buf, 0, descs_buf.bytes};
    const gpu_arena_slice keys_s{&keys, 0, keys.bytes};
    const gpu_arena_slice vals_s{&vals, 0, vals.bytes};
    const gpu_arena_slice keys_scr_s{&keys_scr, 0, keys_scr.bytes};
    const gpu_arena_slice vals_scr_s{&vals_scr, 0, vals_scr.bytes};
    const gpu_arena_slice hist_s{&hist, 0, hist.bytes};
    const gpu_arena_slice index_s{&index_buf, 0, index_buf.bytes};
    const gpu_arena_slice dense_status_s{&dense_status, 0, dense_status.bytes};

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
        const sequence_sort_push_constants dense_push{
            n, params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
            static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
            static_cast<std::uint32_t>(params.used_storage_mask >> 32),
            key_offset, key_offset == 0u ? 0u : 1u, 0u};
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort-dense)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort-dense)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> dense_infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer_handle, descs_s.offset, descs_s.size},
            {index_s.buffer->buffer_handle, index_s.offset, index_s.size},
            {dense_status_s.buffer->buffer_handle, dense_status_s.offset, dense_status_s.size},
        }};
        if (key_offset != 0u) {
            record_compute_dispatch_locked(
                command_buffer_, prefix_pipe, dense_infos, &dense_push, sizeof(dense_push),
                bioseq::common::ceil_div(static_cast<std::uint32_t>(prefix_work), k_sequence_sort_local_size));
            record_compute_barrier(command_buffer_);
        }
        record_compute_dispatch_locked(command_buffer_, dense_pipe, dense_infos,
                                       &dense_push, sizeof(dense_push),
                                       bioseq::common::ceil_div(n, k_sequence_sort_local_size));
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

    const sequence_sort_push_constants key_push{
        n, params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
        static_cast<std::uint32_t>(params.used_storage_mask >> 32), 0u, 0u, 0u};

    // --- Submit 1: extract keys, then radix sort (key,value=row index) ---
    bool sorted_in_scratch = false;
    {
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort1)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort1)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> key_infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer_handle, descs_s.offset, descs_s.size},
            {keys_s.buffer->buffer_handle, keys_s.offset, keys_s.size},
            {vals_s.buffer->buffer_handle, vals_s.offset, vals_s.size},
        }};
        record_compute_dispatch_locked(command_buffer_, key_pipe, key_infos,
                                       &key_push, sizeof(key_push), bioseq::common::ceil_div(n, k_sequence_sort_local_size));
        record_compute_barrier(command_buffer_);
        const gpu_sort_plan sort_plan{n, 8u, 8u, 8u, 8u};   // full 64-bit key, 8 LSD passes
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
    struct run_range { std::uint32_t begin; std::uint32_t len; };
    std::vector<run_range> ranges;
    bool has_oversized_run = false;
    for (std::uint32_t i = 0; i < n; ) {
        std::uint32_t j = i + 1u;
        while (j < n && sorted_keys[j] == sorted_keys[i]) ++j;
        if (j - i > 1u) {
            const std::uint32_t len = j - i;
            ranges.push_back(run_range{i, len});
            has_oversized_run = has_oversized_run || len > k_sequence_sort_local_size;
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
    buffer ranges_buf = create_device_buffer(std::uint64_t{ranges.size()} * 8u, k_compute_buffer_usage);
    if (ranges_buf.mapped == nullptr)
        throw vulkan_runtime_error("run_sequence_sort_roundtrip: ranges buffer alloc failed");
    std::memcpy(ranges_buf.mapped, ranges.data(), ranges.size() * sizeof(run_range));
    const gpu_arena_slice ranges_s{&ranges_buf, 0, ranges_buf.bytes};
    const sequence_sort_push_constants tb_push{
        static_cast<std::uint32_t>(ranges.size()), params.unit_bits, params.bits_per_char,
        params.letters_per_unit, params.char_mask,
        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
        static_cast<std::uint32_t>(params.used_storage_mask >> 32), 0u, 0u, 0u};
    std::uint64_t global_compare_dispatches = 0;
    {
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort2)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort2)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> tb_infos{{
            {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer_handle, descs_s.offset, descs_s.size},
            {index_s.buffer->buffer_handle, index_s.offset, index_s.size},
            {ranges_s.buffer->buffer_handle, ranges_s.offset, ranges_s.size},
        }};
        record_compute_dispatch_locked(command_buffer_, tb_pipe, tb_infos,
                                       &tb_push, sizeof(tb_push),
                                       static_cast<std::uint32_t>(ranges.size()));
        if (has_oversized_run) {
            const std::array<VkDescriptorBufferInfo, 4> gc_infos{{
                {seq_s.buffer->buffer_handle, seq_s.offset, seq_s.size},
                {descs_s.buffer->buffer_handle, descs_s.offset, descs_s.size},
                {index_s.buffer->buffer_handle, index_s.offset, index_s.size},
                {ranges_s.buffer->buffer_handle, ranges_s.offset, ranges_s.size},
            }};
            for (std::uint32_t sort_size = 2u; sort_size <= padded_n; ) {
                for (std::uint32_t stride = sort_size >> 1u; stride > 0u; stride >>= 1u) {
                    record_compute_barrier(command_buffer_);
                    sequence_sort_push_constants gc_push{
                        padded_n, params.unit_bits, params.bits_per_char,
                        params.letters_per_unit, params.char_mask,
                        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
                        static_cast<std::uint32_t>(params.used_storage_mask >> 32),
                        0u, sort_size, stride};
                    record_compute_dispatch_locked(
                        command_buffer_, gc_pipe, gc_infos, &gc_push, sizeof(gc_push),
                        bioseq::common::ceil_div(padded_n, k_sequence_sort_local_size));
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

std::vector<gpu_alignment_extension_result_wire> runtime::run_alignment_extension_roundtrip(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const gpu_alignment_extension_job_wire> jobs,
    const gpu_alignment_extension_params& params)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_alignment_extension_roundtrip on unavailable device");
    }
    if (jobs.empty()) {
        return {};
    }
    validate_alignment_extension_inputs(query_symbols, target_symbols, jobs, params);
    if (features_.max_workgroup_count_x == 0) {
        throw vulkan_runtime_error("alignment extension device reports zero max workgroup count");
    }

    constexpr std::uint32_t k_alignment_extension_local_size = 1u;
    const compute_pipeline pipe = get_compute_pipeline_locked(
        shader_kind::align_extend_banded_affine,
        k_alignment_extension_local_size,
        k_gpu_alignment_extension_max_len);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(query_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(target_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size()) * sizeof(gpu_alignment_extension_job_wire), 4u);
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size()) * sizeof(gpu_alignment_extension_result_wire);

    buffer query_buf = create_device_buffer(query_bytes, k_compute_buffer_usage);
    buffer target_buf = create_device_buffer(target_bytes, k_compute_buffer_usage);
    buffer jobs_buf = create_device_buffer(job_bytes, k_compute_buffer_usage);
    buffer results_buf = create_device_buffer(result_bytes, k_compute_buffer_usage);
    if (query_buf.mapped == nullptr || target_buf.mapped == nullptr
        || jobs_buf.mapped == nullptr || results_buf.mapped == nullptr) {
        throw vulkan_runtime_error("run_alignment_extension_roundtrip: device buffer alloc failed");
    }
    if (!query_symbols.empty()) {
        std::memcpy(query_buf.mapped, query_symbols.data(),
                    query_symbols.size() * sizeof(std::uint32_t));
    }
    if (!target_symbols.empty()) {
        std::memcpy(target_buf.mapped, target_symbols.data(),
                    target_symbols.size() * sizeof(std::uint32_t));
    }
    std::memcpy(jobs_buf.mapped, jobs.data(), jobs.size() * sizeof(gpu_alignment_extension_job_wire));
    std::memset(results_buf.mapped, 0, static_cast<std::size_t>(result_bytes));

    const std::array<VkDescriptorBufferInfo, 4> infos{{
        {query_buf.buffer_handle, 0, query_buf.bytes},
        {target_buf.buffer_handle, 0, target_buf.bytes},
        {jobs_buf.buffer_handle, 0, jobs_buf.bytes},
        {results_buf.buffer_handle, 0, results_buf.bytes},
    }};
    const alignment_extension_push_constants push = alignment_extension_push(
        static_cast<std::uint32_t>(jobs.size()), params);
    const std::uint32_t groups = std::min(
        static_cast<std::uint32_t>(jobs.size()), features_.max_workgroup_count_x);

    reset_dispatch_descriptor_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_ext)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(align_ext)");
    record_host_to_shader(command_buffer_);
    record_compute_dispatch_locked(
        command_buffer_, pipe, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(align_ext)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_ext_post)");

    std::vector<gpu_alignment_extension_result_wire> results(jobs.size());
    std::memcpy(results.data(), results_buf.mapped, static_cast<std::size_t>(result_bytes));
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].status != k_gpu_alignment_extension_status_ok) {
            throw vulkan_runtime_error(fmt::format(
                "alignment extension shader rejected job {} with status {}",
                i, results[i].status));
        }
    }
    alignment_extension_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return results;
}

std::vector<bioseq::prefilter::prefilter_result>
runtime::run_sneaky_snake_rank_batch_roundtrip(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const bioseq::prefilter::prefilter_job> jobs)
{
    std::lock_guard<std::mutex> guard(mu_);
    constexpr std::uint32_t k_sneaky_snake_parallel_threshold = 16u;
    bool use_parallel_teams = false;
    for (const bioseq::prefilter::prefilter_job& job : jobs) {
        if (job.max_edits >= k_sneaky_snake_parallel_threshold
            && job.max_edits <= k_gpu_sneaky_snake_max_edits
            && job.query_length == job.target_length
            && job.query_length != 0u) {
            use_parallel_teams = true;
            break;
        }
    }
    const std::uint32_t push_mode = use_parallel_teams ? 8u : 1u;
    const std::optional<std::size_t> replay_index =
        rank_prefilter_replay_index(
            shader_kind::sneaky_snake_rank_batch, push_mode);
    if (!replay_index) {
        throw vulkan_runtime_error("invalid SneakySnake replay route");
    }
    std::vector<bioseq::prefilter::prefilter_result> results =
        run_rank_prefilter_batch_roundtrip_locked(
            shader_kind::sneaky_snake_rank_batch,
            push_mode,
            *replay_index,
            query_ranks,
            target_ranks,
            jobs);
    if (!jobs.empty()) {
        sneaky_snake_dispatch_count_.fetch_add(
            1u, std::memory_order_relaxed);
    }
    return results;
}

std::vector<bioseq::prefilter::prefilter_result>
runtime::run_gatekeeper_rank_batch_roundtrip(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const bioseq::prefilter::prefilter_job> jobs)
{
    std::lock_guard<std::mutex> guard(mu_);
    const gatekeeper_rank_batch_route route =
        resolve_gatekeeper_rank_batch_route(jobs);
    std::vector<bioseq::prefilter::prefilter_result> results =
        run_rank_prefilter_batch_roundtrip_locked(
            route.kind,
            0u,
            route.replay_index,
            query_ranks,
            target_ranks,
            jobs);
    if (!jobs.empty()) {
        gatekeeper_dispatch_count_.fetch_add(
            1u, std::memory_order_relaxed);
    }
    return results;
}

std::vector<bioseq::prefilter::prefilter_result>
runtime::run_rank_prefilter_batch_roundtrip_locked(
    shader_kind kind,
    std::uint32_t push_mode,
    std::size_t replay_index,
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const bioseq::prefilter::prefilter_job> jobs)
{
    const bool sneaky = kind == shader_kind::sneaky_snake_rank_batch;
    const bool gatekeeper = is_gatekeeper_rank_batch_kind(kind);
    const std::optional<std::size_t> expected_replay_index =
        rank_prefilter_replay_index(kind, push_mode);
    if ((!sneaky && !gatekeeper)
        || replay_index >= prefilter_recorded_commands_.size()
        || !expected_replay_index
        || replay_index != *expected_replay_index) {
        throw vulkan_runtime_error("invalid rank-prefilter dispatch route");
    }
    const char* algorithm = sneaky ? "SneakySnake" : "GateKeeper";
    if (!features_.available) {
        throw vulkan_runtime_error(fmt::format(
            "run_{}_rank_batch_roundtrip on unavailable device",
            sneaky ? "sneaky_snake" : "gatekeeper"));
    }
    if (jobs.empty()) {
        return {};
    }
    validate_prefilter_inputs(
        algorithm, query_ranks, target_ranks, jobs);
    if (features_.max_workgroup_count_x == 0u) {
        throw vulkan_runtime_error(fmt::format(
            "{} device reports zero max workgroup count", algorithm));
    }

    constexpr std::uint32_t k_prefilter_local_size = 64u;
    constexpr std::uint64_t k_retained_workspace_bytes = 64u * 1024u * 1024u;
    const compute_pipeline pipe = get_compute_pipeline_locked(
        kind,
        k_prefilter_local_size,
        sneaky ? k_gpu_sneaky_snake_submaze_length
               : k_gpu_gatekeeper_max_length);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        ((static_cast<std::uint64_t>(query_ranks.size()) + 3u) / 4u) * 4u,
        sizeof(std::uint32_t));
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        ((static_cast<std::uint64_t>(target_ranks.size()) + 3u) / 4u) * 4u,
        sizeof(std::uint32_t));
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size())
            * sizeof(bioseq::prefilter::prefilter_job),
        sizeof(std::uint32_t));
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size())
        * sizeof(bioseq::prefilter::prefilter_result);

    for (const auto [label, bytes] : {
            std::pair{"query ranks", query_bytes},
            std::pair{"target ranks", target_bytes},
            std::pair{"jobs", job_bytes},
            std::pair{"results", result_bytes}}) {
        if (bytes > features_.max_storage_buffer_range) {
            throw vulkan_runtime_error(fmt::format(
                "{} {} buffer {} exceeds maxStorageBufferRange {}",
                algorithm, label, bytes,
                features_.max_storage_buffer_range));
        }
    }

    std::vector<bioseq::prefilter::prefilter_result> results;
    try {
    ensure_prefilter_workspace_buffer_locked(
        prefilter_query_buffer_, query_bytes, k_compute_buffer_usage);
    ensure_prefilter_workspace_buffer_locked(
        prefilter_target_buffer_, target_bytes, k_compute_buffer_usage);
    ensure_prefilter_workspace_buffer_locked(
        prefilter_jobs_buffer_, job_bytes, k_compute_buffer_usage);
    ensure_prefilter_workspace_buffer_locked(
        prefilter_results_buffer_, result_bytes, k_compute_buffer_usage);
    if (prefilter_query_buffer_.mapped == nullptr
        || prefilter_target_buffer_.mapped == nullptr
        || prefilter_jobs_buffer_.mapped == nullptr
        || prefilter_results_buffer_.mapped == nullptr) {
        throw vulkan_runtime_error(fmt::format(
            "run_{}_rank_batch_roundtrip: device buffer alloc failed",
            sneaky ? "sneaky_snake" : "gatekeeper"));
    }

    if constexpr (std::endian::native == std::endian::little) {
        if (!query_ranks.empty()) {
            std::memcpy(
                prefilter_query_buffer_.mapped,
                query_ranks.data(),
                query_ranks.size());
        }
        if (!target_ranks.empty()) {
            std::memcpy(
                prefilter_target_buffer_.mapped,
                target_ranks.data(),
                target_ranks.size());
        }
        const std::size_t query_tail = query_ranks.size() & 3u;
        if (query_tail != 0u || query_ranks.empty()) {
            std::memset(
                static_cast<std::byte*>(prefilter_query_buffer_.mapped)
                    + query_ranks.size(),
                0,
                query_ranks.empty() ? sizeof(std::uint32_t) : 4u - query_tail);
        }
        const std::size_t target_tail = target_ranks.size() & 3u;
        if (target_tail != 0u || target_ranks.empty()) {
            std::memset(
                static_cast<std::byte*>(prefilter_target_buffer_.mapped)
                    + target_ranks.size(),
                0,
                target_ranks.empty() ? sizeof(std::uint32_t) : 4u - target_tail);
        }
    } else {
        std::memset(
            prefilter_query_buffer_.mapped,
            0,
            static_cast<std::size_t>(query_bytes));
        for (std::size_t base = 0u; base < query_ranks.size(); base += 4u) {
            std::uint32_t packed = 0u;
            const std::size_t count =
                std::min<std::size_t>(4u, query_ranks.size() - base);
            for (std::size_t lane = 0u; lane < count; ++lane) {
                packed |= static_cast<std::uint32_t>(query_ranks[base + lane])
                    << (lane * 8u);
            }
            std::memcpy(
                static_cast<std::byte*>(prefilter_query_buffer_.mapped)
                    + base,
                &packed,
                sizeof(packed));
        }
        std::memset(
            prefilter_target_buffer_.mapped,
            0,
            static_cast<std::size_t>(target_bytes));
        for (std::size_t base = 0u; base < target_ranks.size(); base += 4u) {
            std::uint32_t packed = 0u;
            const std::size_t count =
                std::min<std::size_t>(4u, target_ranks.size() - base);
            for (std::size_t lane = 0u; lane < count; ++lane) {
                packed |= static_cast<std::uint32_t>(target_ranks[base + lane])
                    << (lane * 8u);
            }
            std::memcpy(
                static_cast<std::byte*>(prefilter_target_buffer_.mapped)
                    + base,
                &packed,
                sizeof(packed));
        }
    }
    std::memcpy(
        prefilter_jobs_buffer_.mapped,
        jobs.data(),
        jobs.size()
            * sizeof(bioseq::prefilter::prefilter_job));

    const std::array<VkBuffer, 4> descriptor_buffers{{
        prefilter_query_buffer_.buffer_handle,
        prefilter_target_buffer_.buffer_handle,
        prefilter_jobs_buffer_.buffer_handle,
        prefilter_results_buffer_.buffer_handle,
    }};
    prefilter_descriptor_binding_cache& descriptor_binding_cache =
        prefilter_descriptor_binding_cache_[gatekeeper ? 1u : 0u];
    if (descriptor_buffers != descriptor_binding_cache.buffers
        || pipe.descriptor_set != descriptor_binding_cache.descriptor_set) {
        bind_compute_pipeline_descriptors_locked(pipe, descriptor_buffers);
        descriptor_binding_cache.buffers = descriptor_buffers;
        descriptor_binding_cache.descriptor_set = pipe.descriptor_set;
    }
    if ((sneaky && push_mode != 1u && push_mode != 8u)
        || (gatekeeper && push_mode != 0u)) {
        throw vulkan_runtime_error(fmt::format(
            "{} invalid rank-prefilter push mode {}",
            algorithm, push_mode));
    }
    const std::uint32_t jobs_per_workgroup = sneaky
        ? k_prefilter_local_size / push_mode
        : k_prefilter_local_size;
    const gpu_prefilter_push push{
        .job_count = static_cast<std::uint32_t>(jobs.size()),
        .mode = push_mode,
        .query_rank_count = static_cast<std::uint32_t>(query_ranks.size()),
        .target_rank_count = static_cast<std::uint32_t>(target_ranks.size()),
    };
    const std::uint64_t groups_needed =
        (static_cast<std::uint64_t>(jobs.size())
            + jobs_per_workgroup - 1u)
        / jobs_per_workgroup;
    const std::uint32_t groups = static_cast<std::uint32_t>(
        std::min(
            groups_needed,
            static_cast<std::uint64_t>(features_.max_workgroup_count_x)));
    if (groups == 0u) {
        throw vulkan_runtime_error(
            fmt::format(
                "{} grid-stride dispatch has zero workgroups", algorithm));
    }

    prefilter_recorded_command& recorded =
        prefilter_recorded_commands_[replay_index];
    const bool command_matches = recorded.recorded
        && recorded.kind == kind
        && recorded.pipeline == pipe.pipeline
        && recorded.pipeline_layout == pipe.pipeline_layout
        && recorded.descriptor_set == pipe.descriptor_set
        && recorded.push.job_count == push.job_count
        && recorded.push.mode == push.mode
        && recorded.push.query_rank_count == push.query_rank_count
        && recorded.push.target_rank_count == push.target_rank_count
        && recorded.groups == groups;
    if (!command_matches) {
        recorded.recorded = false;
        check_vk(
            vkResetCommandBuffer(recorded.command_buffer, 0),
            "reset(prefilter_cached)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        check_vk(
            vkBeginCommandBuffer(recorded.command_buffer, &begin),
            "begin(prefilter_cached)");
        record_host_to_shader(recorded.command_buffer);
        vkCmdBindPipeline(
            recorded.command_buffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            pipe.pipeline);
        vkCmdBindDescriptorSets(
            recorded.command_buffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            pipe.pipeline_layout,
            0,
            1,
            &pipe.descriptor_set,
            0,
            nullptr);
        vkCmdPushConstants(
            recorded.command_buffer,
            pipe.pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(push),
            &push);
        vkCmdDispatch(recorded.command_buffer, groups, 1u, 1u);
        record_shader_to_host(recorded.command_buffer);
        check_vk(
            vkEndCommandBuffer(recorded.command_buffer),
            "end(prefilter_cached)");
        recorded.kind = kind;
        recorded.pipeline = pipe.pipeline;
        recorded.pipeline_layout = pipe.pipeline_layout;
        recorded.descriptor_set = pipe.descriptor_set;
        recorded.push = push;
        recorded.groups = groups;
        recorded.recorded = true;
    }
    submit_executable_and_wait(
        device_, queue_, recorded.command_buffer, fence_,
        "submit(prefilter_cached)");

    results.resize(jobs.size());
    std::memcpy(
        results.data(),
        prefilter_results_buffer_.mapped,
        jobs.size()
            * sizeof(bioseq::prefilter::prefilter_result));
    if (sneaky) {
        validate_sneaky_snake_results(
            jobs, results, query_ranks.size(), target_ranks.size());
    } else {
        validate_gatekeeper_results(
            jobs, results, query_ranks.size(), target_ranks.size());
    }
    } catch (...) {
        if (prefilter_workspace_allocation_bytes_locked()
            > k_retained_workspace_bytes) {
            // A submit may have succeeded before a wait failed. Do not destroy
            // mapped buffers until the device is known idle (or lost, which
            // also guarantees that no work can continue using them).
            const VkResult idle_result = vkDeviceWaitIdle(device_);
            if (idle_result == VK_SUCCESS
                || idle_result == VK_ERROR_DEVICE_LOST) {
                release_prefilter_workspace_locked();
            }
        }
        throw;
    }
    if (prefilter_workspace_allocation_bytes_locked()
        > k_retained_workspace_bytes) {
        release_prefilter_workspace_locked();
    }
    return results;
}

alignment_cigar_batch_result runtime::run_alignment_simple_cigar_roundtrip(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const gpu_alignment_cigar_job_wire> jobs,
    const gpu_alignment_cigar_params& params,
    std::uint32_t packed_cigar_capacity)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_alignment_simple_cigar_roundtrip on unavailable device");
    }
    if (jobs.empty()) {
        return {};
    }
    validate_alignment_cigar_inputs(
        query_symbols, target_symbols, jobs, params, packed_cigar_capacity);
    if (features_.max_workgroup_count_x == 0) {
        throw vulkan_runtime_error(
            "alignment CIGAR extension device reports zero max workgroup count");
    }

    constexpr std::uint32_t k_alignment_cigar_local_size = 64u;
    const compute_pipeline pipe = get_compute_pipeline_locked(
        shader_kind::align_extend_simple_cigar,
        k_alignment_cigar_local_size,
        k_gpu_alignment_extension_max_len);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(query_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(target_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size()) * sizeof(gpu_alignment_cigar_job_wire), 4u);
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size()) * sizeof(gpu_alignment_cigar_result_wire);
    const std::uint64_t cigar_bytes =
        static_cast<std::uint64_t>(packed_cigar_capacity) * sizeof(std::uint32_t);
    std::uint64_t traceback_words = 0;
    for (const gpu_alignment_cigar_job_wire& job : jobs) {
        const std::uint64_t end =
            static_cast<std::uint64_t>(job.traceback_tile_offset)
            + static_cast<std::uint64_t>(job.traceback_tile_count);
        traceback_words = std::max(traceback_words, end);
    }
    const std::uint64_t traceback_bytes =
        std::max<std::uint64_t>(traceback_words * sizeof(std::uint32_t), 4u);

    buffer query_buf = create_device_buffer(query_bytes, k_compute_buffer_usage);
    buffer target_buf = create_device_buffer(target_bytes, k_compute_buffer_usage);
    buffer jobs_buf = create_device_buffer(job_bytes, k_compute_buffer_usage);
    buffer results_buf = create_device_buffer(result_bytes, k_compute_buffer_usage);
    buffer cigar_buf = create_device_buffer(cigar_bytes, k_compute_buffer_usage);
    buffer traceback_buf = create_device_buffer(traceback_bytes, k_compute_buffer_usage);
    if (query_buf.mapped == nullptr || target_buf.mapped == nullptr
        || jobs_buf.mapped == nullptr || results_buf.mapped == nullptr
        || cigar_buf.mapped == nullptr || traceback_buf.mapped == nullptr) {
        throw vulkan_runtime_error(
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
                jobs.size() * sizeof(gpu_alignment_cigar_job_wire));
    std::memset(results_buf.mapped, 0, static_cast<std::size_t>(result_bytes));
    std::memset(cigar_buf.mapped, 0, static_cast<std::size_t>(cigar_bytes));

    const std::array<VkDescriptorBufferInfo, 6> infos{{
        {query_buf.buffer_handle, 0, query_buf.bytes},
        {target_buf.buffer_handle, 0, target_buf.bytes},
        {jobs_buf.buffer_handle, 0, jobs_buf.bytes},
        {results_buf.buffer_handle, 0, results_buf.bytes},
        {cigar_buf.buffer_handle, 0, cigar_buf.bytes},
        {traceback_buf.buffer_handle, 0, traceback_buf.bytes},
    }};
    const alignment_cigar_push_constants push = alignment_cigar_push(
        static_cast<std::uint32_t>(jobs.size()), params);
    const auto job_count_u64 = static_cast<std::uint64_t>(jobs.size());
    const std::uint64_t groups_u64 =
        (job_count_u64 + k_alignment_cigar_local_size - 1u) / k_alignment_cigar_local_size;
    const std::uint32_t groups = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(groups_u64, features_.max_workgroup_count_x));

    reset_dispatch_descriptor_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_cigar)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(align_cigar)");
    record_host_to_shader(command_buffer_);
    record_compute_dispatch_locked(
        command_buffer_, pipe, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(align_cigar)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_cigar_post)");

    alignment_cigar_batch_result out{};
    out.results.resize(jobs.size());
    out.packed_cigar.resize(packed_cigar_capacity);
    std::memcpy(out.results.data(), results_buf.mapped,
                static_cast<std::size_t>(result_bytes));
    std::memcpy(out.packed_cigar.data(), cigar_buf.mapped,
                static_cast<std::size_t>(cigar_bytes));
    for (std::size_t i = 0; i < out.results.size(); ++i) {
        if (out.results[i].status != k_gpu_alignment_extension_status_ok) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR extension shader rejected job {} with status {}",
                i, out.results[i].status));
        }
    }
    alignment_extension_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

namespace {

constexpr std::uint32_t k_gpu_seed_coordinate_batch_wave_size = 4096u;

struct chain_range_push_constants {
    std::uint32_t query_count;
    std::uint32_t max_ref_gap;
};
static_assert(sizeof(chain_range_push_constants) == 8);

struct gpu_coordinate_vote_job_wire {
    std::uint32_t anchor_offset;
    std::uint32_t anchor_count;
    std::uint32_t query_len_lo;
    std::uint32_t query_len_hi;
};
static_assert(sizeof(gpu_coordinate_vote_job_wire) == 16);

struct gpu_seed_coordinate_wave_state {
    std::size_t result_index = 0;
    gpu_seed_coordinate_job job{};
    gpu_seed_query_record_wire query_record{};
    persisted_grouped_soa_coordinate_job_audit operation_audit{};
    std::uint32_t minimizer_count = 0;
    std::uint32_t num_blocks = 0;
    std::uint32_t seed_groups = 0;
    std::uint64_t query_len = 0;
    gpu_arena_slice counts_s{};
    gpu_arena_slice prefix_s{};
    gpu_arena_slice blocks_s{};
    seed_push_constants seed_push{};
    gpu_grouped_soa_seed_push grouped_soa_seed_push{};
    std::uint64_t total_anchors = 0;
    std::uint32_t total_anchors_u32 = 0;
    std::uint32_t anchor_offset = 0;
    std::uint32_t vote_result_index = 0;
    workgroup_plan anchor_wg{};
    buffer anchors_buf;
    buffer anchor_keys_buf;
    buffer anchors_scratch_buf;
    buffer anchor_keys_scratch_buf;
    buffer anchor_histogram_buf;
    buffer ranges_buf;
    buffer range_count_buf;
    buffer compacted_anchors_buf;
    buffer span_buf;
    bool sorted_in_scratch = false;
    std::uint32_t range_count = 0;
    std::uint32_t compacted_anchor_count = 0;
    std::uint32_t active_anchor_count = 0;
    buffer score_buf;
    buffer predecessor_buf;
    buffer visited_buf;
    buffer trace_buf;
    buffer result_buf;
    bool skip_seed = false;
    bool has_anchors = false;
    bool has_ranges = false;
	bool chain_active = false;

    [[nodiscard]] gpu_arena_slice anchors_slice() noexcept {
        return gpu_arena_slice{&anchors_buf, 0, anchors_buf.bytes};
    }
    [[nodiscard]] gpu_arena_slice anchor_keys_slice() noexcept {
        return gpu_arena_slice{&anchor_keys_buf, 0, anchor_keys_buf.bytes};
    }
    [[nodiscard]] gpu_arena_slice anchors_scratch_slice() noexcept {
        return gpu_arena_slice{&anchors_scratch_buf, 0, anchors_scratch_buf.bytes};
    }
    [[nodiscard]] gpu_arena_slice anchor_keys_scratch_slice() noexcept {
        return gpu_arena_slice{&anchor_keys_scratch_buf, 0, anchor_keys_scratch_buf.bytes};
    }
    [[nodiscard]] gpu_arena_slice anchor_histogram_slice() noexcept {
        return gpu_arena_slice{&anchor_histogram_buf, 0, anchor_histogram_buf.bytes};
    }
    [[nodiscard]] gpu_arena_slice compacted_anchors_slice() noexcept {
        return gpu_arena_slice{&compacted_anchors_buf, 0, compacted_anchors_buf.bytes};
    }
};

[[nodiscard]] bool has_same_coordinate_target(
    const gpu_seed_coordinate_job& lhs,
    const gpu_seed_coordinate_job& rhs) noexcept
{
    return lhs.grouped_soa == rhs.grouped_soa
        && lhs.slots == rhs.slots
        && lhs.positions == rhs.positions
        && lhs.target_len == rhs.target_len
        && lhs.slot_mask == rhs.slot_mask
        && lhs.position_count == rhs.position_count
        && lhs.bucket_count == rhs.bucket_count
        && lhs.target_position_offset == rhs.target_position_offset;
}

}  // namespace

gpu_seed_coordinate_batch_result
runtime::run_seed_to_coordinate_batch_roundtrip(
    const gpu_seed_query_inputs& query_inputs,
    std::span<const gpu_seed_coordinate_job> jobs,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    gpu_seed_coordinate_algorithm coordinate_algorithm,
    const chaining_pipeline_key& chain_key,
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
    device_query_sketch_authorization query_authority)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error(
            "run_seed_to_coordinate_results_roundtrip on unavailable device");
    }
    if (features_.max_workgroup_count_x == 0u) {
        throw vulkan_runtime_error("gpu seed-to-coordinate batch has zero workgroup limit");
    }
    if (coordinate_algorithm != gpu_seed_coordinate_algorithm::vulkan_tiled_dp
        && coordinate_algorithm
            != gpu_seed_coordinate_algorithm::coordinate_vote) {
        throw vulkan_runtime_error(
            "gpu seed-to-coordinate algorithm identity is invalid");
    }
    const bool coordinate_vote_selected =
        coordinate_algorithm
        == gpu_seed_coordinate_algorithm::coordinate_vote;
    if (query_inputs.device_query_records != nullptr) {
        throw vulkan_runtime_error("gpu seed-to-coordinate device query records are not implemented");
    }

    const gpu_seed_index_representation representation = jobs.empty()
        ? gpu_seed_index_representation::invalid
        : classify_gpu_seed_index_representation(jobs.front());
    const bool grouped_soa_selected =
        representation == gpu_seed_index_representation::grouped_soa;
    if (grouped_soa_selected && coordinate_vote_selected) {
        throw vulkan_runtime_error(
            "gpu grouped-SoA direct route requires Vulkan tiled-DP coordinate output");
    }
    if (grouped_soa_selected && features_.software_device) {
        throw vulkan_runtime_error(
            "gpu grouped-SoA direct route rejects software Vulkan devices");
    }
    const auto shape_operation_configuration =
        make_persisted_grouped_soa_coordinate_config_identity(
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
    if (grouped_soa_selected && !shape_operation_configuration.is_valid()) {
        throw vulkan_runtime_error(
            "gpu grouped-SoA coordinate configuration identity is invalid");
    }

    const gpu_grouped_soa_resident_view* common_shape_view = nullptr;
    std::uint64_t common_shape_target_len = 0u;
    for (const gpu_seed_coordinate_job& job : jobs) {
        const gpu_seed_index_representation job_representation =
            classify_gpu_seed_index_representation(job);
        if (job_representation == gpu_seed_index_representation::invalid
            || job_representation != representation) {
            throw vulkan_runtime_error(
                "gpu seed-to-coordinate resident index representation must be strict XOR and uniform");
        }
        if (job.query_index >= query_inputs.host_query_records.size()) {
            throw vulkan_runtime_error("gpu seed-to-coordinate job query_index out of range");
        }
        if (job.target_len == 0u) {
            throw vulkan_runtime_error(
                "gpu seed-to-coordinate target length must be nonzero");
        }
        if (grouped_soa_selected) {
            if (job.target_len > (std::uint64_t{1} << 63u)) {
                throw vulkan_runtime_error(
                    "gpu grouped-SoA target length exceeds anchor-key ABI");
            }
            if (common_shape_view == nullptr) {
                common_shape_view = job.grouped_soa;
                common_shape_target_len = job.target_len;
            } else if (job.grouped_soa != common_shape_view) {
                throw vulkan_runtime_error(
                    "gpu grouped-SoA batch must bind one resident generation");
            } else if (job.target_len != common_shape_target_len) {
                throw vulkan_runtime_error(
                    "gpu grouped-SoA batch target identity is inconsistent");
            }
        }
    }

    gpu_seed_coordinate_batch_result run{};
    run.results.resize(jobs.size());
    run.stats.problem_count = static_cast<std::uint64_t>(jobs.size());
    run.grouped_soa_direct.selected = grouped_soa_selected;
    run.grouped_soa_direct.problem_count = static_cast<std::uint64_t>(jobs.size());

    std::uint32_t shape_control_word_count = 0u;
    std::uint32_t shape_position_word_count = 0u;
    std::uint64_t shape_control_padded_bytes = 0u;
    std::uint64_t shape_position_padded_bytes = 0u;
    if (grouped_soa_selected) {
        if (common_shape_view == nullptr) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA route has no resident generation");
        }
        if (kmer_size == 0u || kmer_size > 255u
            || query_inputs.hash_shift >= 64u
            || query_inputs.pos_bits >= 64u) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA query sketch traits exceed the grouped-SoA ABI");
        }
        const auto valid_resident_buffer = [this](const buffer* buffer) {
            return buffer != nullptr
                && buffer->owner == this
                && buffer->is_valid()
                && buffer->is_host_visible_coherent()
                && buffer->mapped != nullptr;
        };
        if (!valid_resident_buffer(common_shape_view->descriptors)
            || !valid_resident_buffer(common_shape_view->controls)
            || !valid_resident_buffer(common_shape_view->positions)) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA resident buffers are invalid, foreign, or not coherent");
        }
        if (common_shape_view->descriptors->buffer_handle
                == common_shape_view->controls->buffer_handle
            || common_shape_view->descriptors->buffer_handle
                == common_shape_view->positions->buffer_handle
            || common_shape_view->controls->buffer_handle
                == common_shape_view->positions->buffer_handle) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA resident buffers must not alias");
        }
        if (common_shape_view->abi_revision != k_gpu_grouped_soa_abi_revision
            || common_shape_view->partition_count == 0u
            || common_shape_view->control_bytes == 0u
            || common_shape_view->position_bytes == 0u
            || common_shape_view->position_count == 0u
            || common_shape_view->max_occ_cap
                == (std::numeric_limits<std::uint64_t>::max)()
            || common_shape_view->generation_id == 0u
            || common_shape_view->source_identity == 0u
            || common_shape_view->cold_load_identity == 0u) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA resident identity or generation contract is invalid");
        }
        const std::uint64_t expected_descriptor_bytes =
            static_cast<std::uint64_t>(common_shape_view->partition_count)
            * sizeof(gpu_grouped_soa_resident_partition_descriptor);
        if (common_shape_view->descriptor_bytes != expected_descriptor_bytes
            || common_shape_view->descriptor_bytes
                > common_shape_view->descriptors->bytes) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA descriptor byte extent is invalid");
        }
        const auto checked_word_count = [](std::uint64_t bytes,
                                           const char* label) {
            constexpr std::uint64_t maximum_word_bytes =
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)()) * 4u;
            if (bytes == 0u || bytes > maximum_word_bytes) {
                throw vulkan_runtime_error(fmt::format(
                    "gpu grouped-SoA {} byte extent exceeds GLSL word ABI",
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
            throw vulkan_runtime_error(
                "gpu grouped-SoA resident byte extent exceeds its buffer or Vulkan range");
        }
        const bool nil_view_uuid = std::all_of(
            common_shape_view->device_uuid.begin(),
            common_shape_view->device_uuid.end(),
            [](std::uint8_t byte) { return byte == 0u; });
        if (nil_view_uuid
            || common_shape_view->device_uuid != features_.device_uuid) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA resident generation belongs to a different Vulkan device");
        }
        const auto* descriptor_bytes = static_cast<const std::byte*>(
            common_shape_view->descriptors->mapped);
        for (std::uint32_t ordinal = 0u;
             ordinal < common_shape_view->partition_count;
             ++ordinal) {
            gpu_grouped_soa_resident_partition_descriptor descriptor{};
            std::memcpy(
                &descriptor,
                descriptor_bytes
                    + static_cast<std::uint64_t>(ordinal)
                        * sizeof(gpu_grouped_soa_resident_partition_descriptor),
                sizeof(descriptor));
            if (!is_gpu_grouped_soa_partition_descriptor_valid(
                    descriptor,
                    common_shape_view->control_bytes,
                    common_shape_view->position_bytes)
                || descriptor.segment_end_bp > common_shape_target_len) {
                throw vulkan_runtime_error(
                    "gpu grouped-SoA resident descriptor validation failed");
            }
        }
        if (max_occ >= 0
            && static_cast<std::uint64_t>(max_occ)
                > common_shape_view->max_occ_cap) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA query max_occ exceeds persisted generation cap");
        }
        run.grouped_soa_direct.generation_id = common_shape_view->generation_id;
        run.grouped_soa_direct.source_identity = common_shape_view->source_identity;
        run.grouped_soa_direct.cold_load_identity =
            common_shape_view->cold_load_identity;
    }
    const std::uint32_t total_mc = query_inputs.minimizer_count;
    if (jobs.empty() || max_occ < 0
        || (total_mc == 0u && !grouped_soa_selected)) {
        run.grouped_soa_direct.direct_count_scan_emit_completed =
            grouped_soa_selected;
        return run;
    }
    if (query_inputs.host_query_records.empty()) {
        throw vulkan_runtime_error("gpu seed-to-coordinate batch missing query records");
    }
    for (const gpu_seed_query_record_wire& qrec : query_inputs.host_query_records) {
        const std::uint64_t end =
            static_cast<std::uint64_t>(qrec.minimizer_offset)
            + qrec.minimizer_count;
        if (qrec.flags != 0u || qrec.minimizer_offset > total_mc
            || end > total_mc) {
            throw vulkan_runtime_error(
                "gpu seed-to-coordinate query minimizer slice or flags are invalid");
        }
        if (qrec.minimizer_count == 0u) {
            continue;
        }
        if (grouped_soa_selected
            && (qrec.query_len < kmer_size
                || qrec.minimizer_count
                    == (std::numeric_limits<std::uint32_t>::max)()
                || (static_cast<std::uint64_t>(qrec.minimizer_count) + 1u)
                        * sizeof(std::uint32_t)
                    > features_.max_storage_buffer_range)) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA query record violates local count/status ABI");
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
        throw vulkan_runtime_error(
            "gpu seed-to-coordinate minimizer word shape is invalid or overflows");
    }
    const std::uint64_t required_min_words_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(
            query_inputs.minimizer_word_kind,
            query_inputs.minimizer_words32_per_record,
            total_mc),
        4u);
    if (query_inputs.device_minimizer_words != nullptr
        && (!query_inputs.device_minimizer_words->is_valid()
            || query_inputs.device_minimizer_words->owner != this
            || required_min_words_bytes
                > query_inputs.device_minimizer_words->bytes)) {
        throw vulkan_runtime_error(
            "gpu seed-to-coordinate device minimizer buffer is invalid or undersized");
    }
    if (grouped_soa_selected
        && (query_inputs.device_minimizer_words == nullptr
            || !query_inputs.host_minimizer_bytes.empty())) {
        throw vulkan_runtime_error(
            "gpu grouped-SoA direct route requires an unambiguous GPU-resident query sketch");
    }
    std::unique_ptr<runtime_pipeline_use_collector> shape_pipeline_collector;
    if (grouped_soa_selected) {
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
        if (!query_authority.is_valid()
            || query_authority.runtime_ != this
            || query_authority.buffer_
                != query_inputs.device_minimizer_words->buffer_handle
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
            throw vulkan_runtime_error(
                "gpu grouped-SoA direct route lacks matching one-shot query authority");
        }
        if (query_authority.buffer_
                == common_shape_view->descriptors->buffer_handle
            || query_authority.buffer_
                == common_shape_view->controls->buffer_handle
            || query_authority.buffer_
                == common_shape_view->positions->buffer_handle) {
            throw vulkan_runtime_error(
                "gpu grouped-SoA query image aliases resident target storage");
        }
        run.operation_receipt.operation_identity_ =
            next_operation_identity_locked();
        run.operation_receipt.admit_records(jobs.size());
        shape_pipeline_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::grouped_soa_coordinate,
            jobs.size(),
            run.operation_receipt.operation_identity_);
    } else if (query_authority.is_valid()) {
        throw vulkan_runtime_error(
            "gpu query authority cannot be consumed by a source-direct index route");
    }
    const std::uint64_t min_words_bytes =
        query_inputs.device_minimizer_words == nullptr
        ? required_min_words_bytes : 0u;
    if (query_inputs.device_minimizer_words == nullptr
        && query_inputs.host_minimizer_bytes.size()
            > static_cast<std::size_t>(min_words_bytes)) {
        throw vulkan_runtime_error("gpu seed-to-coordinate minimizer upload exceeds buffer");
    }

    const compute_pipeline count_pipe = get_compute_pipeline_locked(
        grouped_soa_selected
            ? shader_kind::grouped_soa_seed_count
            : shader_kind::seed_count,
        128u,
        grouped_soa_selected ? 3u : 1u);
    const compute_pipeline emit_pipe = get_compute_pipeline_locked(
        grouped_soa_selected
            ? shader_kind::grouped_soa_seed_emit
            : shader_kind::seed_emit,
        128u,
        grouped_soa_selected ? 3u : 1u);
    const compute_pipeline scanb =
        get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scana =
        get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
    const compute_pipeline range_pipe =
        get_compute_pipeline_locked(shader_kind::chain_range_build, 1u, 1u);
    const compute_pipeline coord_pipe =
        get_compute_pipeline_locked(shader_kind::coordinate_reduce, 128u, 1u);
    compute_pipeline vote_coord_pipe{};
    compute_pipeline vote_coord_batch_pipe{};
    if (coordinate_vote_selected) {
        vote_coord_pipe = get_compute_pipeline_locked(
            shader_kind::coordinate_vote_reduce, 128u, 1u);
        vote_coord_batch_pipe = get_compute_pipeline_locked(
            shader_kind::coordinate_vote_reduce_batch, 128u, 1u);
    }
    const chaining_pipeline chain_pipeline = create_pipeline_locked(chain_key);
    if (shape_pipeline_collector != nullptr) {
        const auto prepare = [&shape_pipeline_collector](
            const compute_pipeline& pipe) {
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

    constexpr std::uint64_t qrec_stride = k_gpu_buffer_offset_alignment;
    const std::uint64_t query_record_bytes = std::max<std::uint64_t>(
        bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(query_inputs.host_query_records.size()),
            qrec_stride),
        qrec_stride);

    gpu_arena& arena = reusable_gpu_arena();
    std::uint64_t submitted_waves = 0;
    for (std::size_t wave_begin = 0; wave_begin < jobs.size();) {
        std::size_t wave_end =
            std::min<std::size_t>(jobs.size(), wave_begin + k_gpu_seed_coordinate_batch_wave_size);
        if (coordinate_vote_selected && wave_begin + 1u < wave_end) {
            std::size_t same_target_end = wave_begin + 1u;
            while (same_target_end < wave_end
                   && has_same_coordinate_target(
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
            const gpu_seed_query_record_wire& qrec =
                query_inputs.host_query_records[jobs[job_index].query_index];
            const std::uint32_t qblocks =
                std::max<std::uint32_t>(bioseq::common::ceil_div(qrec.minimizer_count, k_prefix_scan_block_size), 1u);
            per_wave_problem_bytes = bioseq::common::saturating_add(
                per_wave_problem_bytes,
                bioseq::common::saturating_add(
                    bioseq::common::saturating_add(
                        std::uint64_t{qrec.minimizer_count} * 8u,
                        grouped_soa_selected ? 4u : 0u),
                    std::uint64_t{qblocks} * 4u));
        }
        const std::uint64_t arena_bytes = bioseq::common::saturating_add(
            bioseq::common::saturating_add(min_words_bytes, query_record_bytes),
            bioseq::common::saturating_add(
                per_wave_problem_bytes,
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(3u * wave_count + 2u),
                    std::uint64_t{k_gpu_buffer_offset_alignment})));
        if (!arena.ensure_capacity(arena_bytes, 0u, 0u)) {
            throw vulkan_runtime_error("gpu seed-to-coordinate batch arena grow failed");
        }
        const gpu_arena_mark mark = arena.mark();
        try {
            auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
            const gpu_arena_slice qrec_s = arena.allocate_device(query_record_bytes, 16u);
            gpu_arena_slice minw_s{};
            if (query_inputs.device_minimizer_words == nullptr) {
                minw_s = arena.allocate_device(min_words_bytes, 16u);
            }
            if (!qrec_s.is_valid()
                || (query_inputs.device_minimizer_words == nullptr && !minw_s.is_valid())) {
                throw vulkan_runtime_error("gpu seed-to-coordinate batch arena oversubscribed");
            }

            const VkBuffer min_words_buffer = query_inputs.device_minimizer_words != nullptr
                ? query_inputs.device_minimizer_words->buffer_handle
                : minw_s.buffer->buffer_handle;
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
                    sizeof(gpu_seed_query_record_wire));
            }

            std::vector<gpu_seed_coordinate_wave_state> states;
            states.reserve(wave_count);
            std::uint32_t seed_active_count = 0;
            for (std::size_t job_index = wave_begin; job_index < wave_end; ++job_index) {
                gpu_seed_coordinate_wave_state& state = states.emplace_back();
                state.result_index = job_index;
                state.job = jobs[job_index];
                state.query_record =
                    query_inputs.host_query_records[state.job.query_index];
                if (grouped_soa_selected) {
                    auto& audit = state.operation_audit;
                    audit.operation_identity =
                        run.operation_receipt.operation_identity_;
                    audit.query_authority_identity =
                        query_authority.identity_;
                    audit.resident_descriptors = make_runtime_buffer_identity(
                        common_shape_view->descriptors);
                    audit.resident_controls = make_runtime_buffer_identity(
                        common_shape_view->controls);
                    audit.resident_positions = make_runtime_buffer_identity(
                        common_shape_view->positions);
                    audit.query_minimizer_buffer = make_runtime_buffer_identity(
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
                    audit.abi_revision = common_shape_view->abi_revision;
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
                        query_authority.dispatch_stats_.count_dispatches();
                    audit.query_scan_dispatches =
                        query_authority.dispatch_stats_.prefix_scan_dispatches();
                    audit.query_emit_dispatches =
                        query_authority.dispatch_stats_.emit_dispatches();
                    audit.query_completed_roundtrips =
                        query_authority.dispatch_stats_.completed_roundtrips();
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
                    bioseq::common::ceil_div(state.minimizer_count, k_prefix_scan_block_size), 1u);
                state.seed_groups = std::max<std::uint32_t>(
                    1u,
                    grouped_soa_selected
                        ? 1u
                        : std::min(
                              bioseq::common::ceil_div(state.minimizer_count, 128u),
                              features_.max_workgroup_count_x));
                state.skip_seed = state.minimizer_count == 0u
                    || (!grouped_soa_selected && state.job.position_count == 0u);
                if (grouped_soa_selected && state.minimizer_count == 0u) {
                    run.operation_receipt.records_[state.result_index].audit =
                        state.operation_audit;
                }
                const std::uint64_t count_word_count = grouped_soa_selected
                    ? static_cast<std::uint64_t>(state.minimizer_count) + 1u
                    : std::max<std::uint64_t>(state.minimizer_count, 1u);
                state.counts_s = arena.allocate_device(
                    count_word_count * 4u, 16u);
                state.prefix_s = arena.allocate_device(
                    std::max<std::uint64_t>(state.minimizer_count, 1u) * 4u, 16u);
                state.blocks_s = arena.allocate_device(
                    std::uint64_t{state.num_blocks} * 4u, 16u);
                if (!state.counts_s.is_valid() || !state.prefix_s.is_valid()
                    || !state.blocks_s.is_valid()) {
                    throw vulkan_runtime_error(
                        "gpu seed-to-coordinate batch arena oversubscribed");
                }
                if (grouped_soa_selected) {
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
                const gpu_anchor_sort_key_plan anchor_sort_plan =
                    make_anchor_sort_key_plan(
                        state.job.target_len,
                        state.job.target_position_offset);
                state.seed_push = seed_push_constants{
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
                if (grouped_soa_selected) {
                    const gpu_grouped_soa_resident_view& view =
                        *state.job.grouped_soa;
                    state.grouped_soa_seed_push = gpu_grouped_soa_seed_push{
                        .abi_revision = k_gpu_grouped_soa_abi_revision,
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
                std::vector<gpu_seed_coordinate_wave_state*> active_states;
                active_states.reserve(seed_active_count);
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (!state.skip_seed) {
                        active_states.push_back(&state);
                    }
                }

                const gpu_seed_coordinate_wave_state* first_active =
                    active_states.empty() ? nullptr : active_states.front();
                bool same_target_wave = active_states.size() >= 2u;
                if (same_target_wave) {
                    for (const gpu_seed_coordinate_wave_state* state : active_states) {
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
                            bioseq::common::ceil_div(total_mc, k_prefix_scan_block_size),
                            1u);
                    const std::uint64_t counts_bytes =
                        std::max<std::uint64_t>(std::uint64_t{total_mc} * 4u, 4u);
                    const std::uint64_t blocks_bytes =
                        std::max<std::uint64_t>(std::uint64_t{total_blocks} * 4u, 4u);
                    const std::uint64_t dense_qrec_bytes =
                        std::max<std::uint64_t>(
                            static_cast<std::uint64_t>(active_states.size())
                                * sizeof(gpu_seed_query_record_wire),
                            sizeof(gpu_seed_query_record_wire));

                    buffer batch_counts_buf =
                        create_device_buffer(counts_bytes, k_compute_buffer_usage);
                    buffer batch_prefix_buf =
                        create_device_buffer(counts_bytes, k_compute_buffer_usage);
                    buffer batch_blocks_buf =
                        create_device_buffer(blocks_bytes, k_compute_buffer_usage);
                    buffer batch_qrec_buf =
                        create_device_buffer(dense_qrec_bytes, k_compute_buffer_usage);
                    if (batch_counts_buf.mapped == nullptr
                        || batch_prefix_buf.mapped == nullptr
                        || batch_blocks_buf.mapped == nullptr
                        || batch_qrec_buf.mapped == nullptr) {
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate multi-record batch non-unified memory");
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
                                + active_index * sizeof(gpu_seed_query_record_wire),
                            &active_states[active_index]->query_record,
                            sizeof(gpu_seed_query_record_wire));
                    }

                    seed_push_constants batch_seed_push = first_active->seed_push;
                    batch_seed_push.record_count =
                        static_cast<std::uint32_t>(active_states.size());
                    batch_seed_push.emit_mode = 0u;
                    const std::array<VkDescriptorBufferInfo, 8> count_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {first_active->job.slots->buffer_handle, 0, VK_WHOLE_SIZE},
                        {first_active->job.positions->buffer_handle, 0, VK_WHOLE_SIZE},
                        {batch_qrec_buf.buffer_handle, 0, batch_qrec_buf.bytes},
                        {batch_counts_buf.buffer_handle, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer_handle, 0, batch_prefix_buf.bytes},
                        {batch_counts_buf.buffer_handle, 0, batch_counts_buf.bytes},
                        {batch_counts_buf.buffer_handle, 0, batch_counts_buf.bytes},
                    }};
                    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                        {batch_counts_buf.buffer_handle, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer_handle, 0, batch_prefix_buf.bytes},
                        {batch_blocks_buf.buffer_handle, 0, batch_blocks_buf.bytes},
                    }};
                    scan_push_constants scan{
                        total_mc, k_prefix_scan_block_size, total_blocks, 0u};

                    reset_dispatch_descriptor_pool_locked();
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
                    record_compute_dispatch_locked(
                        command_buffer_, count_pipe, count_infos,
                        &batch_seed_push, sizeof(batch_seed_push),
                        static_cast<std::uint32_t>(active_states.size()));
                    run.stats.seed_count_dispatches += 1u;
                    record_compute_barrier(command_buffer_);
                    record_compute_dispatch_locked(
                        command_buffer_, scanb, scan_infos,
                        &scan, sizeof(scan), total_blocks);
                    record_compute_barrier(command_buffer_);
                    record_compute_dispatch_locked(
                        command_buffer_, scana, scan_infos,
                        &scan, sizeof(scan), 1u);
                    record_compute_barrier(command_buffer_);
                    scan.pass = 1u;
                    record_compute_dispatch_locked(
                        command_buffer_, scana, scan_infos,
                        &scan, sizeof(scan), bioseq::common::ceil_div(total_mc, 128u));
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
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate multi-record anchor overflow");
                    }

                    std::uint32_t anchor_active_count = 0;
                    for (gpu_seed_coordinate_wave_state* state : active_states) {
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
                            throw vulkan_runtime_error(
                                "gpu seed-to-coordinate multi-record invalid prefix");
                        }
                        state->total_anchors =
                            anchor_end - static_cast<std::uint64_t>(anchor_begin);
                        if (state->total_anchors == 0u) {
                            continue;
                        }
                        if (state->total_anchors
                            > std::numeric_limits<std::uint32_t>::max()) {
                            throw vulkan_runtime_error(
                                "gpu seed-to-coordinate multi-record per-query overflow");
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
                    buffer batch_anchors_buf =
                        create_device_buffer(anchor_bytes, k_compute_buffer_usage);
                    buffer batch_anchor_keys_buf = create_device_buffer(
                        wave_total_anchors * sizeof(std::uint64_t),
                        k_compute_buffer_usage);
                    if (batch_anchors_buf.mapped == nullptr
                        || batch_anchor_keys_buf.mapped == nullptr) {
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate multi-record anchor memory");
                    }
                    std::memset(
                        batch_anchors_buf.mapped, 0,
                        static_cast<std::size_t>(batch_anchors_buf.bytes));
                    std::memset(
                        batch_anchor_keys_buf.mapped, 0,
                        static_cast<std::size_t>(batch_anchor_keys_buf.bytes));

                    std::vector<gpu_coordinate_vote_job_wire> vote_jobs;
                    vote_jobs.reserve(anchor_active_count);
                    for (gpu_seed_coordinate_wave_state* state : active_states) {
                        if (!state->has_anchors) {
                            continue;
                        }
                        state->vote_result_index =
                            static_cast<std::uint32_t>(vote_jobs.size());
                        vote_jobs.push_back(gpu_coordinate_vote_job_wire{
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
                                * sizeof(gpu_coordinate_vote_job_wire),
                            sizeof(gpu_coordinate_vote_job_wire));
                    const std::uint64_t vote_result_bytes =
                        std::max<std::uint64_t>(
                            static_cast<std::uint64_t>(vote_jobs.size())
                                * sizeof(gpu_coordinate_result_wire),
                            sizeof(gpu_coordinate_result_wire));
                    buffer vote_jobs_buf =
                        create_device_buffer(vote_job_bytes, k_compute_buffer_usage);
                    buffer vote_results_buf =
                        create_device_buffer(vote_result_bytes, k_compute_buffer_usage);
                    if (vote_jobs_buf.mapped == nullptr
                        || vote_results_buf.mapped == nullptr) {
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate multi-record vote memory");
                    }
                    std::memset(
                        vote_jobs_buf.mapped, 0,
                        static_cast<std::size_t>(vote_jobs_buf.bytes));
                    std::memset(
                        vote_results_buf.mapped, 0,
                        static_cast<std::size_t>(vote_results_buf.bytes));
                    std::memcpy(
                        vote_jobs_buf.mapped, vote_jobs.data(),
                        vote_jobs.size() * sizeof(gpu_coordinate_vote_job_wire));

                    reset_dispatch_descriptor_pool_locked();
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
                        {first_active->job.slots->buffer_handle, 0, VK_WHOLE_SIZE},
                        {first_active->job.positions->buffer_handle, 0, VK_WHOLE_SIZE},
                        {batch_qrec_buf.buffer_handle, 0, batch_qrec_buf.bytes},
                        {batch_counts_buf.buffer_handle, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer_handle, 0, batch_prefix_buf.bytes},
                        {batch_anchors_buf.buffer_handle, 0, batch_anchors_buf.bytes},
                        {batch_anchor_keys_buf.buffer_handle, 0, batch_anchor_keys_buf.bytes},
                    }};
                    record_compute_dispatch_locked(
                        command_buffer_, emit_pipe, emit_infos,
                        &batch_seed_push, sizeof(batch_seed_push),
                        static_cast<std::uint32_t>(active_states.size()));
                    run.stats.seed_emit_dispatches += 1u;
                    record_compute_barrier(command_buffer_);
                    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                        {batch_anchors_buf.buffer_handle, 0, batch_anchors_buf.bytes},
                        {vote_jobs_buf.buffer_handle, 0, vote_jobs_buf.bytes},
                        {batch_anchor_keys_buf.buffer_handle, 0, batch_anchor_keys_buf.bytes},
                        {vote_results_buf.buffer_handle, 0, vote_results_buf.bytes},
                        {vote_results_buf.buffer_handle, 0, vote_results_buf.bytes},
                        {vote_results_buf.buffer_handle, 0, vote_results_buf.bytes},
                    }};
                    gpu_coordinate_reduce_push coord_push{
                        .anchor_count = static_cast<std::uint32_t>(vote_jobs.size()),
                        .query_len_lo = 0u,
                        .query_len_hi = 0u,
                        .min_chain_score = min_chain_score,
                        .min_seed_count = min_seed_count,
                        .probe_limit = probe_limit,
                        .reserved = 0u,
                    };
                    record_compute_dispatch_locked(
                        command_buffer_, vote_coord_batch_pipe, coord_infos,
                        &coord_push, sizeof(coord_push),
                        static_cast<std::uint32_t>(vote_jobs.size()));
                    run.stats.coordinate_vote_dispatches += 1u;
                    record_shader_to_host(command_buffer_);
                    submit_and_wait(
                        device_, queue_, command_buffer_, fence_,
                        "submit(pc_seed_coord_mrec2)");

                    for (gpu_seed_coordinate_wave_state* state : active_states) {
                        if (state->has_anchors) {
                            std::memcpy(
                                &run.results[state->result_index],
                                static_cast<std::byte*>(vote_results_buf.mapped)
                                    + static_cast<std::uint64_t>(
                                        state->vote_result_index)
                                        * sizeof(gpu_coordinate_result_wire),
                                sizeof(gpu_coordinate_result_wire));
                        }
                    }

                    arena.rewind(mark);
                    submitted_waves += 1u;
                    run.stats.wave_count += 1u;
                    wave_begin = next_wave_begin;
                    continue;
                }
            }

            reset_dispatch_descriptor_pool_locked();
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
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                if (grouped_soa_selected) {
                    const gpu_grouped_soa_resident_view& view =
                        *state.job.grouped_soa;
                    const std::array<VkDescriptorBufferInfo, 9> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {view.descriptors->buffer_handle, 0, view.descriptor_bytes},
                        {view.controls->buffer_handle, 0, shape_control_padded_bytes},
                        {view.positions->buffer_handle, 0, shape_position_padded_bytes},
                        {qrec_s.buffer->buffer_handle,
                         qrec_s.offset + std::uint64_t{state.job.query_index}
                             * qrec_stride,
                         sizeof(gpu_seed_query_record_wire)},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer_handle,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                    }};
                    record_compute_dispatch_locked(
                        command_buffer_, count_pipe, seed_infos,
                        &state.grouped_soa_seed_push,
                        sizeof(state.grouped_soa_seed_push),
                        1u, shape_pipeline_collector.get(),
                        state.result_index);
                    run.grouped_soa_direct.count_dispatches += 1u;
                    state.operation_audit.direct_count_dispatches += 1u;
                } else {
                    const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {state.job.slots->buffer_handle, 0, VK_WHOLE_SIZE},
                        {state.job.positions->buffer_handle, 0, VK_WHOLE_SIZE},
                        {qrec_s.buffer->buffer_handle,
                         qrec_s.offset + std::uint64_t{state.job.query_index}
                             * qrec_stride,
                         sizeof(gpu_seed_query_record_wire)},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer_handle,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                    }};
                    record_compute_dispatch_locked(
                        command_buffer_, count_pipe, seed_infos,
                        &state.seed_push, sizeof(state.seed_push),
                        state.seed_groups);
                }
                run.stats.seed_count_dispatches += 1u;
            }
            record_compute_barrier(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer_handle, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer_handle, state.prefix_s.offset, state.prefix_s.size},
                    {state.blocks_s.buffer->buffer_handle, state.blocks_s.offset, state.blocks_s.size},
                }};
                scan_push_constants scan{
                    state.minimizer_count, k_prefix_scan_block_size, state.num_blocks, 0u};
                record_compute_dispatch_locked(
                    command_buffer_, scanb, scan_infos, &scan, sizeof(scan),
                    state.num_blocks, shape_pipeline_collector.get(),
                    state.result_index);
                if (grouped_soa_selected) {
                    state.operation_audit.direct_scan_dispatches += 1u;
                }
            }
            record_compute_barrier(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer_handle, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer_handle, state.prefix_s.offset, state.prefix_s.size},
                    {state.blocks_s.buffer->buffer_handle, state.blocks_s.offset, state.blocks_s.size},
                }};
                scan_push_constants scan{
                    state.minimizer_count, k_prefix_scan_block_size, state.num_blocks, 0u};
                record_compute_dispatch_locked(
                    command_buffer_, scana, scan_infos, &scan, sizeof(scan),
                    1u, shape_pipeline_collector.get(), state.result_index);
                if (grouped_soa_selected) {
                    state.operation_audit.direct_scan_dispatches += 1u;
                }
            }
            record_compute_barrier(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer_handle, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer_handle, state.prefix_s.offset, state.prefix_s.size},
                    {state.blocks_s.buffer->buffer_handle, state.blocks_s.offset, state.blocks_s.size},
                }};
                scan_push_constants scan{
                    state.minimizer_count, k_prefix_scan_block_size, state.num_blocks, 1u};
                record_compute_dispatch_locked(
                    command_buffer_, scana, scan_infos, &scan, sizeof(scan),
                    bioseq::common::ceil_div(state.minimizer_count, 128u),
                    shape_pipeline_collector.get(), state.result_index);
                if (grouped_soa_selected) {
                    state.operation_audit.direct_scan_dispatches += 1u;
                }
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(
                device_, queue_, command_buffer_, fence_,
                "submit(pc_seed_coord_batch1)",
                shape_pipeline_collector.get());
            if (grouped_soa_selected) {
                const std::uint64_t receipt_identity =
                    next_operation_identity_locked();
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (!state.skip_seed) {
                        state.operation_audit.count_submit_wait_identity =
                            receipt_identity;
                    }
                }
            }

            std::uint32_t anchor_active_count = 0;
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                if (grouped_soa_selected) {
                    std::uint32_t status = 0u;
                    std::memcpy(
                        &status,
                        base + state.counts_s.offset
                            + static_cast<std::uint64_t>(state.minimizer_count)
                                * sizeof(std::uint32_t),
                        sizeof(status));
                    run.grouped_soa_direct.count_status_checks += 1u;
                    run.grouped_soa_direct.status_or |= status;
                    state.operation_audit.count_status_checks += 1u;
                    state.operation_audit.status_or |= status;
                    if (status != 0u) {
                        throw vulkan_runtime_error(fmt::format(
                            "gpu grouped-SoA count rejected job {} with status 0x{:08x}",
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
                if (grouped_soa_selected) {
                    state.operation_audit.anchor_count =
                        state.total_anchors;
                    shape_pipeline_collector->set_shape_outcome(
                        state.result_index, state.total_anchors, 0u);
                }
                if (state.total_anchors == 0u) {
                    if (grouped_soa_selected) {
                        run.operation_receipt.records_[state.result_index].audit =
                            state.operation_audit;
                    }
                    continue;
                }
                if (state.total_anchors > std::numeric_limits<std::uint32_t>::max()) {
                    throw vulkan_runtime_error(
                        "gpu seed-to-coordinate batch anchor count overflow");
                }
                state.total_anchors_u32 = static_cast<std::uint32_t>(state.total_anchors);
                state.anchor_wg =
                    plan_workgroups(state.total_anchors, features_.max_workgroup_count_x);
                const std::uint64_t anchor_u64 = state.total_anchors * 3u;
                state.anchors_buf =
                    create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
                state.anchor_keys_buf =
                    create_device_buffer(state.total_anchors * 8u, k_compute_buffer_usage);
                if (coordinate_vote_selected) {
                    state.result_buf = create_device_buffer(
                        sizeof(gpu_coordinate_result_wire), k_compute_buffer_usage);
                    if (state.anchors_buf.mapped == nullptr
                        || state.anchor_keys_buf.mapped == nullptr
                        || state.result_buf.mapped == nullptr) {
                        throw vulkan_runtime_error(
                            "gpu seed-to-coordinate vote batch non-unified memory");
                    }
                    std::memset(
                        state.result_buf.mapped, 0,
                        static_cast<std::size_t>(state.result_buf.bytes));
                    state.has_anchors = true;
                    anchor_active_count += 1u;
                    continue;
                }
                state.anchors_scratch_buf =
                    create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
                state.anchor_keys_scratch_buf =
                    create_device_buffer(state.total_anchors * 8u, k_compute_buffer_usage);
                state.anchor_histogram_buf = create_device_buffer(
                    std::uint64_t{state.anchor_wg.num_workgroups} * 256u * 4u,
                    k_compute_buffer_usage);
                state.ranges_buf = create_device_buffer(
                    std::max<std::uint64_t>(
                        state.total_anchors * sizeof(gpu_chain_range_wire), 8u),
                    k_compute_buffer_usage);
                state.range_count_buf = create_device_buffer(8u, k_compute_buffer_usage);
                state.compacted_anchors_buf =
                    create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
                state.span_buf =
                    create_device_buffer(sizeof(gpu_chain_range_build_span_wire), k_compute_buffer_usage);
                if (state.anchors_buf.mapped == nullptr
                    || state.anchor_keys_buf.mapped == nullptr
                    || state.anchors_scratch_buf.mapped == nullptr
                    || state.anchor_keys_scratch_buf.mapped == nullptr
                    || state.anchor_histogram_buf.mapped == nullptr
                    || state.ranges_buf.mapped == nullptr
                    || state.range_count_buf.mapped == nullptr
                    || state.compacted_anchors_buf.mapped == nullptr
                    || state.span_buf.mapped == nullptr) {
                    throw vulkan_runtime_error(
                        "gpu seed-to-coordinate batch non-unified anchor memory");
                }
                const gpu_chain_range_build_span_wire span{
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

            reset_dispatch_descriptor_pool_locked();
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
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                gpu_arena_slice anchors_s = state.anchors_slice();
                gpu_arena_slice anchor_keys_s = state.anchor_keys_slice();
                if (grouped_soa_selected) {
                    gpu_grouped_soa_seed_push emit_push = state.grouped_soa_seed_push;
                    emit_push.emit_mode = 1u;
                    emit_push.anchor_capacity = state.total_anchors_u32;
                    const gpu_grouped_soa_resident_view& view =
                        *state.job.grouped_soa;
                    const std::array<VkDescriptorBufferInfo, 9> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {view.descriptors->buffer_handle, 0, view.descriptor_bytes},
                        {view.controls->buffer_handle, 0, shape_control_padded_bytes},
                        {view.positions->buffer_handle, 0, shape_position_padded_bytes},
                        {qrec_s.buffer->buffer_handle,
                         qrec_s.offset + std::uint64_t{state.job.query_index}
                             * qrec_stride,
                         sizeof(gpu_seed_query_record_wire)},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer_handle,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {anchors_s.buffer->buffer_handle,
                         anchors_s.offset,
                         anchors_s.size},
                        {anchor_keys_s.buffer->buffer_handle,
                         anchor_keys_s.offset,
                         anchor_keys_s.size},
                    }};
                    record_compute_dispatch_locked(
                        command_buffer_, emit_pipe, seed_infos,
                        &emit_push, sizeof(emit_push), 1u,
                        shape_pipeline_collector.get(), state.result_index);
                    run.grouped_soa_direct.emit_dispatches += 1u;
                    state.operation_audit.direct_emit_dispatches += 1u;
                } else {
                    seed_push_constants emit_push = state.seed_push;
                    emit_push.emit_mode = 1u;
                    const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {state.job.slots->buffer_handle, 0, VK_WHOLE_SIZE},
                        {state.job.positions->buffer_handle, 0, VK_WHOLE_SIZE},
                        {qrec_s.buffer->buffer_handle,
                         qrec_s.offset + std::uint64_t{state.job.query_index}
                             * qrec_stride,
                         sizeof(gpu_seed_query_record_wire)},
                        {state.counts_s.buffer->buffer_handle,
                         state.counts_s.offset,
                         state.counts_s.size},
                        {state.prefix_s.buffer->buffer_handle,
                         state.prefix_s.offset,
                         state.prefix_s.size},
                        {anchors_s.buffer->buffer_handle,
                         anchors_s.offset,
                         anchors_s.size},
                        {anchor_keys_s.buffer->buffer_handle,
                         anchor_keys_s.offset,
                         anchor_keys_s.size},
                    }};
                    record_compute_dispatch_locked(
                        command_buffer_, emit_pipe, seed_infos,
                        &emit_push, sizeof(emit_push), state.seed_groups);
                }
                run.stats.seed_emit_dispatches += 1u;
            }
            record_compute_barrier(command_buffer_);
            if (grouped_soa_selected) {
                record_shader_to_host(command_buffer_);
                submit_and_wait(
                    device_, queue_, command_buffer_, fence_,
                    "submit(pc_grouped_soa_seed_coord_emit)",
                    shape_pipeline_collector.get());
                const std::uint64_t receipt_identity =
                    next_operation_identity_locked();
                for (gpu_seed_coordinate_wave_state& state : states) {
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
                    run.grouped_soa_direct.emit_status_checks += 1u;
                    run.grouped_soa_direct.status_or |= status;
                    state.operation_audit.emit_status_checks += 1u;
                    state.operation_audit.status_or |= status;
                    if (status != 0u) {
                        throw vulkan_runtime_error(fmt::format(
                            "gpu grouped-SoA emit rejected job {} with status 0x{:08x}",
                            state.result_index,
                            status));
                    }
                }

                reset_dispatch_descriptor_pool_locked();
                shape_pipeline_collector->burn_before_reset(command_buffer_);
                check_vk(
                    vkResetCommandBuffer(command_buffer_, 0),
                    "reset(pc_grouped_soa_seed_coord_post_emit)");
                begin = VkCommandBufferBeginInfo{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                check_vk(
                    vkBeginCommandBuffer(command_buffer_, &begin),
                    "begin(pc_grouped_soa_seed_coord_post_emit)");
                shape_pipeline_collector->begin(command_buffer_);
            }
            if (coordinate_vote_selected) {
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (!state.has_anchors) {
                        continue;
                    }
                    gpu_arena_slice anchors_s = state.anchors_slice();
                    gpu_arena_slice anchor_keys_s = state.anchor_keys_slice();
                    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                        {anchors_s.buffer->buffer_handle, anchors_s.offset, anchors_s.size},
                        {anchor_keys_s.buffer->buffer_handle, anchor_keys_s.offset, anchor_keys_s.size},
                        {anchor_keys_s.buffer->buffer_handle, anchor_keys_s.offset, anchor_keys_s.size},
                        {state.result_buf.buffer_handle, 0, state.result_buf.bytes},
                        {state.result_buf.buffer_handle, 0, state.result_buf.bytes},
                        {state.result_buf.buffer_handle, 0, state.result_buf.bytes},
                    }};
                    gpu_coordinate_reduce_push coord_push{
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
                    record_compute_dispatch_locked(
                        command_buffer_, vote_coord_pipe, coord_infos,
                        &coord_push, sizeof(coord_push), 1u);
                    run.stats.coordinate_vote_dispatches += 1u;
                }
                record_shader_to_host(command_buffer_);
                submit_and_wait(
                    device_, queue_, command_buffer_, fence_,
                    "submit(pc_seed_coord_vote_batch2)");

                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (state.has_anchors) {
                        std::memcpy(
                            &run.results[state.result_index],
                            state.result_buf.mapped,
                            sizeof(gpu_coordinate_result_wire));
                    }
                }

                arena.rewind(mark);
                submitted_waves += 1u;
                run.stats.wave_count += 1u;
                wave_begin = next_wave_begin;
                continue;
            }
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                gpu_arena_slice anchors_s = state.anchors_slice();
                gpu_arena_slice anchor_keys_s = state.anchor_keys_slice();
                gpu_arena_slice anchors_scratch_s = state.anchors_scratch_slice();
                gpu_arena_slice anchor_keys_scratch_s = state.anchor_keys_scratch_slice();
                gpu_arena_slice anchor_histogram_s = state.anchor_histogram_slice();
                const gpu_anchor_sort_key_plan anchor_sort_plan =
                    make_anchor_sort_key_plan(
                        state.job.target_len,
                        state.job.target_position_offset);
                gpu_sort_plan sort_plan{
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
                if (grouped_soa_selected) {
                    state.operation_audit.sort_operations += 1u;
                }
                const gpu_arena_slice sorted_anchors =
                    state.sorted_in_scratch ? anchors_scratch_s : anchors_s;
                const gpu_arena_slice compacted_anchors_s = state.compacted_anchors_slice();
                const std::array<VkDescriptorBufferInfo, 6> range_infos{{
                    {sorted_anchors.buffer->buffer_handle, sorted_anchors.offset, sorted_anchors.size},
                    {state.range_count_buf.buffer_handle, 0, state.range_count_buf.bytes},
                    {state.ranges_buf.buffer_handle, 0, state.ranges_buf.bytes},
                    {state.range_count_buf.buffer_handle, 0, state.range_count_buf.bytes},
                    {compacted_anchors_s.buffer->buffer_handle, compacted_anchors_s.offset,
                     compacted_anchors_s.size},
                    {state.span_buf.buffer_handle, 0, state.span_buf.bytes},
                }};
                chain_range_push_constants range_push{
                    .query_count = 1u,
                    .max_ref_gap =
                        static_cast<std::uint32_t>(std::max<std::int32_t>(max_dist_x, 0)),
                };
                record_compute_dispatch_locked(
                    command_buffer_, range_pipe, range_infos,
                    &range_push, sizeof(range_push), 1u,
                    shape_pipeline_collector.get(), state.result_index);
                run.stats.range_build_dispatches += 1u;
                if (grouped_soa_selected) {
                    state.operation_audit.range_build_dispatches += 1u;
                }
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(
                device_, queue_, command_buffer_, fence_,
                "submit(pc_seed_coord_batch2)",
                shape_pipeline_collector.get());
            if (grouped_soa_selected) {
                const std::uint64_t receipt_identity =
                    next_operation_identity_locked();
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (state.has_anchors) {
                        state.operation_audit.sort_range_submit_wait_identity =
                            receipt_identity;
                    }
                }
            }

            std::uint32_t chain_active_count = 0;
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                std::memcpy(&state.range_count, state.range_count_buf.mapped, 4u);
                std::memcpy(
                    &state.compacted_anchor_count,
                    static_cast<std::byte*>(state.range_count_buf.mapped) + 4u,
                    4u);
                if (grouped_soa_selected) {
                    state.operation_audit.range_count = state.range_count;
                    shape_pipeline_collector->set_shape_outcome(
                        state.result_index,
                        state.total_anchors,
                        state.range_count);
                }
                if (state.range_count == 0u) {
                    if (grouped_soa_selected) {
                        run.operation_receipt.records_[state.result_index].audit =
                            state.operation_audit;
                    }
                    continue;
                }
                if (state.compacted_anchor_count > state.total_anchors_u32) {
                    throw vulkan_runtime_error(
                        "gpu seed-to-coordinate batch compacted anchor overflow");
                }
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u
                    && state.compacted_anchor_count < state.total_anchors_u32;
                state.active_anchor_count =
                    use_compacted_anchors ? state.compacted_anchor_count
                                          : state.total_anchors_u32;
                state.score_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::int32_t),
                    k_compute_buffer_usage);
                state.predecessor_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::int64_t),
                    k_compute_buffer_usage);
                state.visited_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::uint32_t),
                    k_compute_buffer_usage);
                state.trace_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::uint32_t),
                    k_compute_buffer_usage);
                state.result_buf =
                    create_device_buffer(sizeof(gpu_coordinate_result_wire), k_compute_buffer_usage);
                if (state.score_buf.mapped == nullptr
                    || state.predecessor_buf.mapped == nullptr
                    || state.visited_buf.mapped == nullptr
                    || state.trace_buf.mapped == nullptr
                    || state.result_buf.mapped == nullptr) {
                    throw vulkan_runtime_error(
                        "gpu seed-to-coordinate batch non-unified chain memory");
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

            reset_dispatch_descriptor_pool_locked();
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
            for (gpu_seed_coordinate_wave_state& state : states) {
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
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                gpu_arena_slice anchors_s = state.anchors_slice();
                gpu_arena_slice anchors_scratch_s = state.anchors_scratch_slice();
                gpu_arena_slice compacted_anchors_s = state.compacted_anchors_slice();
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u
                    && state.compacted_anchor_count < state.total_anchors_u32;
                const gpu_arena_slice active_anchors = use_compacted_anchors
                    ? compacted_anchors_s
                    : (state.sorted_in_scratch ? anchors_scratch_s : anchors_s);
                const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
                    {active_anchors.buffer->buffer_handle, active_anchors.offset, active_anchors.size},
                    {state.ranges_buf.buffer_handle, 0, state.ranges_buf.bytes},
                    {state.score_buf.buffer_handle, 0, state.score_buf.bytes},
                    {state.predecessor_buf.buffer_handle, 0, state.predecessor_buf.bytes},
                }};
                VkDescriptorSet chain_set = allocate_dispatch_descriptor_set_locked(descriptor_set_layout_);
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

                chaining_push_constants chain_push{};
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
                if (grouped_soa_selected) {
                    state.operation_audit.chain_dispatches += 1u;
                }
            }
            record_compute_barrier(command_buffer_);
            for (gpu_seed_coordinate_wave_state& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                gpu_arena_slice anchors_s = state.anchors_slice();
                gpu_arena_slice anchors_scratch_s = state.anchors_scratch_slice();
                gpu_arena_slice compacted_anchors_s = state.compacted_anchors_slice();
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u
                    && state.compacted_anchor_count < state.total_anchors_u32;
                const gpu_arena_slice active_anchors = use_compacted_anchors
                    ? compacted_anchors_s
                    : (state.sorted_in_scratch ? anchors_scratch_s : anchors_s);
                const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                    {active_anchors.buffer->buffer_handle, active_anchors.offset, active_anchors.size},
                    {state.score_buf.buffer_handle, 0, state.score_buf.bytes},
                    {state.predecessor_buf.buffer_handle, 0, state.predecessor_buf.bytes},
                    {state.visited_buf.buffer_handle, 0, state.visited_buf.bytes},
                    {state.trace_buf.buffer_handle, 0, state.trace_buf.bytes},
                    {state.result_buf.buffer_handle, 0, state.result_buf.bytes},
                }};
                gpu_coordinate_reduce_push coord_push{
                    .anchor_count = state.active_anchor_count,
                    .query_len_lo = static_cast<std::uint32_t>(state.query_len & 0xffffffffull),
                    .query_len_hi = static_cast<std::uint32_t>(state.query_len >> 32),
                    .min_chain_score = min_chain_score,
                    .min_seed_count = min_seed_count,
                    .probe_limit = probe_limit,
                    .reserved = 0u,
                };
                record_compute_dispatch_locked(
                    command_buffer_, coord_pipe, coord_infos,
                    &coord_push, sizeof(coord_push), 1u,
                    shape_pipeline_collector.get(), state.result_index);
                run.stats.chain_backtrack_dispatches += 1u;
                if (grouped_soa_selected) {
                    state.operation_audit.chain_backtrack_dispatches += 1u;
                    state.operation_audit.coordinate_dispatches += 1u;
                }
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(
                device_, queue_, command_buffer_, fence_,
                "submit(pc_seed_coord_batch3)",
                shape_pipeline_collector.get());
            if (grouped_soa_selected) {
                const std::uint64_t receipt_identity =
                    next_operation_identity_locked();
                for (gpu_seed_coordinate_wave_state& state : states) {
                    if (state.chain_active) {
                        state.operation_audit
                            .chain_coordinate_submit_wait_identity =
                            receipt_identity;
                    }
                }
            }

            for (gpu_seed_coordinate_wave_state& state : states) {
                if (state.chain_active) {
                    std::memcpy(
                        &run.results[state.result_index],
                        state.result_buf.mapped,
                        sizeof(gpu_coordinate_result_wire));
                    if (grouped_soa_selected) {
                        state.operation_audit.result =
                            run.results[state.result_index];
                        state.operation_audit.result_readbacks += 1u;
                        run.operation_receipt.records_[state.result_index].audit =
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
    if (grouped_soa_selected) {
        run.grouped_soa_direct.direct_count_scan_emit_completed = true;
        run.operation_receipt.query_pipeline_token_ =
            std::move(query_authority.pipeline_token_);
        run.operation_receipt.grouped_soa_pipeline_token_ =
            shape_pipeline_collector->seal();
        run.operation_receipt.sealed_ =
            run.operation_receipt.records_.size() == jobs.size()
            && !run.operation_receipt.records_.empty()
            && run.operation_receipt.has_valid_causal_tokens()
            && std::all_of(
                run.operation_receipt.records_.begin(),
                run.operation_receipt.records_.end(),
                [](const auto& record) {
                    return record.state ==
                            persisted_grouped_soa_coordinate_operation_receipt::
                                record_state::fresh
                        && record.audit.is_internally_consistent();
                });
    }
    return run;
}

runtime_detail::persisted_grouped_soa_coordinate_finalization_plan
runtime::prepare_persisted_grouped_soa_coordinate_finalization(
    const persisted_grouped_soa_coordinate_operation_receipt& receipt,
    const persisted_grouped_soa_coordinate_operation_receipt_preview&
        expected_preview,
    const vulkan_feature_set& live_features,
    const bool seed_metrics_claimed) noexcept {
    runtime_detail::persisted_grouped_soa_coordinate_finalization_plan result{};
    const persisted_grouped_soa_coordinate_operation_receipt_preview receipt_preview =
        receipt.operation_receipt_preview();
    if (!receipt.are_all_jobs_accepted() || !expected_preview.is_internally_consistent()
        || !receipt_preview.is_internally_consistent()
        || receipt_preview != expected_preview) {
        return result;
    }
    const runtime_gpu_execution_environment_snapshot environment_preview =
        receipt.environment_preview();
    const runtime_gpu_cache_metrics_audit cache_metrics_maximum =
        receipt.cache_metrics_preview();
    if (!environment_preview.is_internally_consistent()
        || !cache_metrics_maximum.is_internally_consistent()) {
        return result;
    }
    const runtime_pipeline_operation_audit& query =
        receipt.query_pipeline_token_.audit();
    const runtime_pipeline_operation_audit& shape =
        receipt.grouped_soa_pipeline_token_.audit();
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

    const auto union_receipts = receipt.receipt_union();
    if (!union_receipts.valid) return result;
    result.cache_claim_plan = runtime_detail::
        runtime_gpu_cache_metrics_claim_plan::prepare(
            receipt.operation_identity_, cache_metrics_maximum,
            seed_metrics_claimed,
            std::span<const runtime_metal_archive_pipeline_receipt* const>{
                union_receipts.receipts.data(), union_receipts.count});
    if (!result.cache_claim_plan.is_valid()) return {};

    vulkan_feature_set identity_features{};
    identity_features.api_version = query.api_version;
    identity_features.driver_version = query.driver_version;
    identity_features.vendor_id = query.vendor_id;
    identity_features.device_id = query.device_id;
    identity_features.pipeline_cache_uuid = query.pipeline_cache_uuid;
    result.environment = runtime_gpu_execution_environment_snapshot{
        .gpu_environment_present = true,
        .gpu_device_uuid = query.device_uuid,
        .gpu_driver_identity =
            canonical_vulkan_driver_identity(identity_features),
        .cache_policy = runtime_pipeline_cache_policy::
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
        persisted_grouped_soa_coordinate_operation_acceptance_audit{
            .operation_identity = receipt.operation_identity_,
            .expected_job_count =
                static_cast<std::uint64_t>(receipt.records_.size()),
            .consumed_count =
                static_cast<std::uint64_t>(receipt.consumed_count()),
            .accepted_count =
                static_cast<std::uint64_t>(receipt.accepted_count()),
            .burned_count =
                static_cast<std::uint64_t>(receipt.burned_count()),
            .query_pipeline_receipt_count =
                static_cast<std::uint64_t>(query.prepared_count),
            .shape_pipeline_receipt_count =
                static_cast<std::uint64_t>(shape.prepared_count),
            .committed_submission_count = query.committed_submissions
                + shape.committed_submissions,
            .seed_receipt_identity = query.seed_receipt_identity,
        };
    if (!result.environment.is_internally_consistent()
        || result.environment != environment_preview
        || !result.acceptance.is_internally_consistent()
        || result.acceptance != expected_preview.acceptance) {
        return {};
    }
    result.valid = true;
    return result;
}

bool runtime::self_test_persisted_grouped_soa_finalizer_preclaim_contract(
    runtime_metal_archive_pipeline_audit base_audit) {
    if (!base_audit.is_internally_consistent() || base_audit.pipeline_create_ns == 0u) {
        return false;
    }
    const std::array<shader_variant_key, 11> keys{
        make_shader_key(
            shader_kind::ordered_sketch_tile, 384u, 64u, 10u),
        make_shader_key(
            shader_kind::prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::prefix_scan_add, 384u, 128u, 1u),
        make_shader_key(
            shader_kind::grouped_soa_seed_count, 384u, 128u, 3u),
        make_shader_key(
            shader_kind::grouped_soa_seed_emit, 384u, 128u, 3u),
        make_shader_key(
            shader_kind::chain_range_build, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::coordinate_reduce, 384u, 128u, 1u),
        make_shader_key(
            shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u),
        make_shader_key(
            shader_kind::radix_sort_histogram, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::radix_sort_scan, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::radix_sort_scatter, 384u, 1u, 1u),
    };
    if (!std::all_of(
            keys.begin(), keys.end(), [](const shader_variant_key& key) {
                return is_shader_variant_key_valid(key);
            })) {
        return false;
    }
    std::array<runtime_metal_archive_pipeline_receipt, 11> receipts{};
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        runtime_metal_archive_pipeline_audit audit = base_audit;
        audit.receipt_identity = 8000u + i;
        audit.scope_identity = 8100u + i;
        audit.shader_key = keys[i];
        receipts[i] = runtime_metal_archive_pipeline_receipt{
            std::move(audit)};
        if (!receipts[i].is_valid()) return false;
    }
    const auto command = [](const std::uintptr_t identity) {
        return reinterpret_cast<VkCommandBuffer>(identity);
    };

    runtime_pipeline_operation_token query{};
    runtime_pipeline_operation_token shape{};
    try {
        auto query_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::query_sketch, 1u, 101u);
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

        auto shape_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::grouped_soa_coordinate, 1u, 202u);
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
    if (!query.is_valid_for_query_sketch()
        || !shape.is_valid_for_grouped_soa_coordinate()) {
        return false;
    }

    persisted_grouped_soa_coordinate_operation_receipt receipt{};
    receipt.operation_identity_ = 202u;
    try {
        receipt.admit_records(1u);
    } catch (...) {
        return false;
    }
    receipt.records_[0].state =
        persisted_grouped_soa_coordinate_operation_receipt::record_state::accepted;
    receipt.accepted_count_ = 1u;
    receipt.query_pipeline_token_ = std::move(query);
    receipt.grouped_soa_pipeline_token_ = std::move(shape);
    receipt.sealed_ = true;
    const persisted_grouped_soa_coordinate_operation_receipt_preview expected =
        receipt.operation_receipt_preview();
    if (!receipt.are_all_jobs_accepted() || !expected.is_internally_consistent()
        || expected.acceptance.shape_pipeline_receipt_count != 10u) {
        return false;
    }
    vulkan_feature_set live_features{};
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
        auto accepted = prepare_persisted_grouped_soa_coordinate_finalization(
            receipt, expected, live_features, seed_claimed);
        if (!accepted.is_valid()
            || accepted.acceptance != expected.acceptance
            || accepted.environment != receipt.environment_preview()
            || accepted.cache_claim_plan.audit()
                != expected.maximum_cache_metrics
            || accepted.cache_claim_plan.is_committed()) {
            return false;
        }
    }
    if (!claims_unchanged()) return false;

    const auto rejected = [&](const auto& candidate,
                              const vulkan_feature_set& live) {
        auto plan = prepare_persisted_grouped_soa_coordinate_finalization(
            receipt, candidate, live, seed_claimed);
        return !plan.is_valid() && claims_unchanged();
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
        runtime_metal_archive_pipeline_audit saved = receipts[10].audit_;
        mutate(receipts[10].audit_);
        // Receipt mutations exercise the finalizer's frozen PREPARED/USED
        // equality gate, not the qualification's own self-consistency gate.
        // Keep the process qualification bound to the mutated live audit so
        // the receipt remains independently accepted before the finalizer
        // rejects its drift from the sealed operation receipt.
        runtime_metal_archive_process_qualification& qualification =
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
        const bool remained_valid = receipts[10].is_valid();
        const bool result = remained_valid
            && rejected(expected, live_features);
        receipts[10].audit_ = std::move(saved);
        return result && claims_unchanged();
    };
    const auto change_nonzero = [](auto& value) {
        using value_type = std::remove_reference_t<decltype(value)>;
        value = value == (std::numeric_limits<value_type>::max)()
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

bool runtime::self_test_gpu_cache_partial_delta_contract(
    runtime_metal_archive_pipeline_audit base_audit) {
    if (!base_audit.is_internally_consistent() || base_audit.pipeline_create_ns == 0u) {
        return false;
    }
    const std::array<shader_variant_key, 3> keys{
        make_shader_key(
            shader_kind::ordered_sketch_tile, 384u, 64u, 10u),
        make_shader_key(
            shader_kind::prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::prefix_scan_add, 384u, 128u, 1u),
    };
    if (!std::all_of(
            keys.begin(), keys.end(), [](const shader_variant_key& key) {
                return is_shader_variant_key_valid(key);
            })) {
        return false;
    }
    std::array<runtime_metal_archive_pipeline_receipt, 3> receipts{};
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        runtime_metal_archive_pipeline_audit audit = base_audit;
        audit.receipt_identity = 7000u + i;
        audit.scope_identity = 7100u + i;
        audit.shader_key = keys[i];
        receipts[i] = runtime_metal_archive_pipeline_receipt{
            std::move(audit)};
        if (!receipts[i].is_valid()) return false;
    }
    std::array<const runtime_metal_archive_pipeline_receipt*, 3> pointers{};
    runtime_gpu_cache_metrics_audit maximum{
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
    if (!maximum.is_internally_consistent()) return false;

    bool seed_claimed = false;
    runtime_gpu_cache_metrics_audit subset_maximum{
        .token_identity = 7199u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = receipts[0].audit().seed_size,
        .gpu_pipeline_cache_load_ns = receipts[0].audit().seed_load_ns,
        .gpu_pipeline_creates = 1u,
        .gpu_pipeline_creation_ns =
            receipts[0].audit().pipeline_create_ns,
    };
    auto subset = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        subset_maximum.token_identity, subset_maximum, seed_claimed,
        std::span<const runtime_metal_archive_pipeline_receipt* const>{
            pointers.data(), 1u});
    if (!subset.is_valid() || subset.audit() != subset_maximum) {
        return false;
    }
    subset.commit(seed_claimed);
    if (!subset.is_committed() || !seed_claimed
        || !receipts[0].metrics_claimed_
        || receipts[1].metrics_claimed_
        || receipts[2].metrics_claimed_) {
        return false;
    }

    auto remainder = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        maximum.token_identity, maximum, seed_claimed,
        std::span<const runtime_metal_archive_pipeline_receipt* const>{
            pointers.data(), pointers.size()});
    if (!remainder.is_valid()) return false;
    const runtime_gpu_cache_metrics_audit partial = remainder.audit();
    if (!partial.is_componentwise_bounded_by(maximum)
        || partial == maximum
        || partial.gpu_pipeline_cache_loads != 0u
        || partial.gpu_pipeline_cache_seed_bytes != 0u
        || partial.gpu_pipeline_cache_load_ns != 0u
        || partial.gpu_pipeline_creates != 2u
        || partial.gpu_pipeline_creation_ns == 0u) {
        return false;
    }
    remainder.commit(seed_claimed);
    if (!remainder.is_committed()
        || std::any_of(
            receipts.begin(), receipts.end(), [](const auto& receipt) {
                return !receipt.metrics_claimed_;
            })) {
        return false;
    }

    runtime_gpu_cache_metrics_delta issued{};
    issued.audit_ = partial;
    issued.issued_ = true;
    runtime_gpu_cache_metrics_delta moved{std::move(issued)};
    if (issued.is_valid() || issued.audit() != runtime_gpu_cache_metrics_audit{}
        || !moved.is_valid() || moved.audit() != partial) {
        return false;
    }
    moved.consume();
    if (moved.is_valid()
        || moved.audit() != runtime_gpu_cache_metrics_audit{}) {
        return false;
    }

    runtime_gpu_cache_metrics_audit later_maximum = maximum;
    later_maximum.token_identity = 7201u;
    auto later = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        later_maximum.token_identity, later_maximum, seed_claimed,
        std::span<const runtime_metal_archive_pipeline_receipt* const>{
            pointers.data(), pointers.size()});
    const runtime_gpu_cache_metrics_audit zero = later.audit();
    if (!later.is_valid()
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
    if (!later.is_committed()) return false;
    runtime_gpu_cache_metrics_delta zero_delta{};
    zero_delta.audit_ = zero;
    zero_delta.issued_ = true;
    if (!zero_delta.is_valid()) return false;
    zero_delta.consume();
    return !zero_delta.is_valid()
        && zero_delta.audit() == runtime_gpu_cache_metrics_audit{};
}

persisted_grouped_soa_coordinate_accepted_operation
runtime::finalize_persisted_grouped_soa_coordinate_operation(
    persisted_grouped_soa_coordinate_operation_receipt&& receipt,
    const persisted_grouped_soa_coordinate_operation_receipt_preview&
        expected_preview) {
    std::scoped_lock lock(mu_);
    auto plan = prepare_persisted_grouped_soa_coordinate_finalization(
        receipt, expected_preview, features_, seed_metrics_claimed_);
    if (!plan.is_valid()) {
        throw vulkan_runtime_error(
            "persisted grouped-SoA operation finalization payload is invalid");
    }

    // This is deliberately the last external-state check. Everything after it
    // is fixed-field move/copy construction and cannot dispatch or allocate.
    require_immutable_metal_policy_live(
        "persisted grouped-SoA operation final publication");

    // No failing work follows this mutation. The queue/cache mutex makes the
    // seed and receipt claims one transaction with authority minting.
    const runtime_gpu_cache_metrics_audit cache_metrics =
        plan.cache_claim_plan.audit();
    plan.cache_claim_plan.commit(seed_metrics_claimed_);
    persisted_grouped_soa_coordinate_accepted_operation accepted{};
    accepted.audit_ = plan.acceptance;
    accepted.environment_ = plan.environment;
    accepted.cache_metrics_.audit_ = cache_metrics;
    accepted.cache_metrics_.issued_ = true;
    accepted.issued_ = true;
    receipt = persisted_grouped_soa_coordinate_operation_receipt{};
    return accepted;
}

std::vector<gpu_coordinate_result_wire>
runtime::run_seed_to_coordinate_results_roundtrip(
    const gpu_seed_query_inputs& query_inputs,
    std::span<const gpu_seed_coordinate_job> jobs,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    gpu_seed_coordinate_algorithm coordinate_algorithm,
    const chaining_pipeline_key& chain_key,
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
        throw vulkan_runtime_error("gpu seed-to-coordinate query length exceeds ABI");
    }
    const gpu_seed_query_record_wire qrec{
        .minimizer_offset = 0u,
        .minimizer_count = query_inputs.minimizer_count,
        .query_len = static_cast<std::uint32_t>(query_len),
        .flags = 0u,
    };
    gpu_seed_query_inputs forwarded = query_inputs;
    forwarded.host_query_records =
        std::span<const gpu_seed_query_record_wire>{&qrec, 1u};
    forwarded.device_query_records = nullptr;
    return run_seed_to_coordinate_batch_roundtrip(
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

gpu_coordinate_result_wire runtime::run_seed_to_coordinate_roundtrip(
    const gpu_seed_inputs& inputs,
    std::uint64_t query_len,
    std::uint64_t target_len,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    const chaining_pipeline_key& chain_key,
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
    gpu_coordinate_result_wire result{};
    if (!features_.available) {
        throw vulkan_runtime_error("run_seed_to_coordinate_roundtrip on unavailable device");
    }
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw vulkan_runtime_error("gpu seed-to-coordinate missing resident index buffers");
    }
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error("gpu seed-to-coordinate query length exceeds ABI");
    }
    const std::uint32_t mc = inputs.minimizer_count;
    if (mc == 0u || max_occ < 0) return result;

    struct chain_range_push_constants {
        std::uint32_t query_count;
        std::uint32_t max_ref_gap;
    };
    static_assert(sizeof(chain_range_push_constants) == 8);

    gpu_arena& arena = reusable_gpu_arena();
    const std::uint32_t num_blocks = bioseq::common::ceil_div(mc, k_prefix_scan_block_size);
    const std::uint64_t min_words_bytes = inputs.device_minimizer_words == nullptr
        ? std::max<std::uint64_t>(
              minimizer_device_bytes(
                  inputs.minimizer_word_kind, inputs.minimizer_words32_per_record, mc),
              4u)
        : 0u;
    std::uint64_t dt = std::uint64_t{mc} * 4u * 2u + std::uint64_t{num_blocks} * 4u
        + 16u + min_words_bytes + 6u * std::uint64_t{k_gpu_buffer_offset_alignment};
    if (!arena.ensure_capacity(dt, 0, 0)) {
        throw vulkan_runtime_error("gpu seed-to-coordinate arena grow failed");
    }
    const gpu_arena_mark mark = arena.mark();

    const gpu_arena_slice counts_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const gpu_arena_slice prefix_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const gpu_arena_slice blocks_s = arena.allocate_device(
        std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const gpu_arena_slice qrec_s = arena.allocate_device(16u, 16u);
    gpu_arena_slice minw_s{};
    if (inputs.device_minimizer_words == nullptr) {
        minw_s = arena.allocate_device(min_words_bytes, 16u);
    }
    if (!counts_s.is_valid() || !prefix_s.is_valid() || !blocks_s.is_valid() || !qrec_s.is_valid()
        || (inputs.device_minimizer_words == nullptr && !minw_s.is_valid())) {
        arena.rewind(mark);
        throw vulkan_runtime_error("gpu seed-to-coordinate arena oversubscribed");
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    const VkBuffer min_words_buffer = inputs.device_minimizer_words != nullptr
        ? inputs.device_minimizer_words->buffer_handle
        : minw_s.buffer->buffer_handle;
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
    const host_query_record qrec{0u, mc, static_cast<std::uint32_t>(query_len), 0u};
    std::memcpy(base + qrec_s.offset, &qrec, sizeof(qrec));

    const compute_pipeline count_pipe =
        get_compute_pipeline_locked(shader_kind::seed_count, 128u, 1u);
    const compute_pipeline emit_pipe =
        get_compute_pipeline_locked(shader_kind::seed_emit, 128u, 1u);
    const compute_pipeline scanb =
        get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scana =
        get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
    const compute_pipeline range_pipe =
        get_compute_pipeline_locked(shader_kind::chain_range_build, 1u, 1u);
    const compute_pipeline coord_pipe =
        get_compute_pipeline_locked(shader_kind::coordinate_reduce, 128u, 1u);
    const VkPipeline chain_pipeline =
        create_pipeline_locked(chain_key).pipeline;

    const std::uint32_t target_offset_lo =
        static_cast<std::uint32_t>(inputs.target_position_offset & 0xffffffffull);
    const std::uint32_t target_offset_hi =
        static_cast<std::uint32_t>(inputs.target_position_offset >> 32);
    const gpu_anchor_sort_key_plan anchor_sort_plan =
        make_anchor_sort_key_plan(
            inputs.target_len != 0u ? inputs.target_len : target_len,
            inputs.target_position_offset);
    seed_push_constants cpush{inputs.minimizer_words32_per_record, inputs.minimizer_word_kind,
        inputs.hash_shift, inputs.pos_bits, kmer_size, inputs.slot_mask,
        static_cast<std::uint32_t>(max_occ), 0u, 1u, inputs.bucket_count,
        target_offset_lo, target_offset_hi, anchor_sort_plan.target_ref_bits};
    const std::uint32_t seed_groups = std::max<std::uint32_t>(
        1u,
        std::min(bioseq::common::ceil_div(mc, 128u), features_.max_workgroup_count_x));

    {
        reset_dispatch_descriptor_pool_locked();
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer_handle, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer_handle, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer_handle, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
        }};
        const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {blocks_s.buffer->buffer_handle, blocks_s.offset, blocks_s.size},
        }};
        scan_push_constants s0{mc, k_prefix_scan_block_size, num_blocks, 0u};
        scan_push_constants s1{mc, k_prefix_scan_block_size, num_blocks, 1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord1)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord1)");
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, count_pipe, seed_infos, &cpush, sizeof(cpush), seed_groups);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, scanb, scan_infos, &s0, sizeof(s0), num_blocks);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, scana, scan_infos, &s0, sizeof(s0), 1u);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, scana, scan_infos, &s1, sizeof(s1), bioseq::common::ceil_div(mc, 128u));
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
        throw vulkan_runtime_error("gpu seed-to-coordinate anchor count overflow");
    }
    const auto total_u32 = static_cast<std::uint32_t>(total);

    const std::uint64_t anchor_u64 = total * 3u;
    const workgroup_plan awg = plan_workgroups(total, features_.max_workgroup_count_x);
    buffer anchors_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer akeys_buf = create_device_buffer(total * 8u, k_compute_buffer_usage);
    buffer anchors_scr_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer akeys_scr_buf = create_device_buffer(total * 8u, k_compute_buffer_usage);
    buffer ahist_buf = create_device_buffer(
        std::uint64_t{awg.num_workgroups} * 256u * 4u, k_compute_buffer_usage);
    buffer ranges_buf = create_device_buffer(
        std::max<std::uint64_t>(total * sizeof(gpu_chain_range_wire), 8u),
        k_compute_buffer_usage);
    buffer range_count_buf = create_device_buffer(8u, k_compute_buffer_usage);
    buffer compacted_anchors_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer span_buf = create_device_buffer(sizeof(gpu_chain_range_build_span_wire), k_compute_buffer_usage);
    if (anchors_buf.mapped == nullptr || ranges_buf.mapped == nullptr
        || range_count_buf.mapped == nullptr || compacted_anchors_buf.mapped == nullptr
        || span_buf.mapped == nullptr) {
        arena.rewind(mark);
        throw vulkan_runtime_error("gpu seed-to-coordinate non-unified memory");
    }
    const gpu_arena_slice anchors_s{&anchors_buf, 0, anchors_buf.bytes};
    const gpu_arena_slice akeys_s{&akeys_buf, 0, akeys_buf.bytes};
    const gpu_arena_slice anchors_scr_s{&anchors_scr_buf, 0, anchors_scr_buf.bytes};
    const gpu_arena_slice akeys_scr_s{&akeys_scr_buf, 0, akeys_scr_buf.bytes};
    const gpu_arena_slice ahist_s{&ahist_buf, 0, ahist_buf.bytes};
    const gpu_arena_slice compacted_anchors_s{
        &compacted_anchors_buf, 0, compacted_anchors_buf.bytes};

    const gpu_chain_range_build_span_wire span{
        .anchor_lo = 0u,
        .anchor_hi = total_u32,
        .range_lo = 0u,
        .range_out = 0u,
    };
    std::memcpy(span_buf.mapped, &span, sizeof(span));
    std::memset(range_count_buf.mapped, 0, 8u);

    bool sorted_in_scratch = false;
    {
        reset_dispatch_descriptor_pool_locked();
        seed_push_constants epush = cpush;
        epush.emit_mode = 1u;
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer_handle, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer_handle, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer_handle, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {anchors_s.buffer->buffer_handle, anchors_s.offset, anchors_s.size},
            {akeys_s.buffer->buffer_handle, akeys_s.offset, akeys_s.size},
        }};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord2)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord2)");
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, emit_pipe, seed_infos, &epush, sizeof(epush), seed_groups);
        record_compute_barrier(command_buffer_);
        gpu_sort_plan sort_plan{total, 8u, 24u, 8u, anchor_sort_plan.pass_count};
        record_radix_sort_locked(
            command_buffer_, akeys_s, anchors_s, akeys_scr_s, anchors_scr_s, ahist_s,
            sort_plan, awg.num_workgroups, awg.elems_per_wg, 3u, sorted_in_scratch);
        const gpu_arena_slice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
        const std::array<VkDescriptorBufferInfo, 6> range_infos{{
            {sorted_anchors.buffer->buffer_handle, sorted_anchors.offset, sorted_anchors.size},
            {range_count_buf.buffer_handle, 0, range_count_buf.bytes},
            {ranges_buf.buffer_handle, 0, ranges_buf.bytes},
            {range_count_buf.buffer_handle, 0, range_count_buf.bytes},
            {compacted_anchors_s.buffer->buffer_handle, compacted_anchors_s.offset,
             compacted_anchors_s.size},
            {span_buf.buffer_handle, 0, span_buf.bytes},
        }};
        chain_range_push_constants rpush{
            .query_count = 1u,
            .max_ref_gap = static_cast<std::uint32_t>(std::max<std::int32_t>(max_dist_x, 0)),
        };
        record_compute_dispatch_locked(
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
        throw vulkan_runtime_error("gpu seed-to-coordinate compacted anchor overflow");
    }
    const bool use_compacted_anchors =
        compacted_anchor_count != 0u && compacted_anchor_count < total_u32;
    const std::uint32_t active_anchor_count =
        use_compacted_anchors ? compacted_anchor_count : total_u32;

    buffer score_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::int32_t), k_compute_buffer_usage);
    buffer pred_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::int64_t), k_compute_buffer_usage);
    buffer visited_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::uint32_t), k_compute_buffer_usage);
    buffer trace_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::uint32_t), k_compute_buffer_usage);
    buffer result_buf = create_device_buffer(sizeof(gpu_coordinate_result_wire), k_compute_buffer_usage);
    const gpu_arena_slice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
    const gpu_arena_slice& active_anchors =
        use_compacted_anchors ? compacted_anchors_s : sorted_anchors;

    {
        reset_dispatch_descriptor_pool_locked();
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
            {active_anchors.buffer->buffer_handle, active_anchors.offset, active_anchors.size},
            {ranges_buf.buffer_handle, 0, ranges_buf.bytes},
            {score_buf.buffer_handle, 0, score_buf.bytes},
            {pred_buf.buffer_handle, 0, pred_buf.bytes},
        }};
        VkDescriptorSet chain_set = allocate_dispatch_descriptor_set_locked(descriptor_set_layout_);
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

        chaining_push_constants chain_push{};
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
            {active_anchors.buffer->buffer_handle, active_anchors.offset, active_anchors.size},
            {score_buf.buffer_handle, 0, score_buf.bytes},
            {pred_buf.buffer_handle, 0, pred_buf.bytes},
            {visited_buf.buffer_handle, 0, visited_buf.bytes},
            {trace_buf.buffer_handle, 0, trace_buf.bytes},
            {result_buf.buffer_handle, 0, result_buf.bytes},
        }};
        gpu_coordinate_reduce_push coord_push{
            .anchor_count = active_anchor_count,
            .query_len_lo = static_cast<std::uint32_t>(query_len & 0xffffffffull),
            .query_len_hi = static_cast<std::uint32_t>(query_len >> 32),
            .min_chain_score = min_chain_score,
            .min_seed_count = min_seed_count,
            .probe_limit = probe_limit,
            .reserved = 0u,
        };
        record_compute_dispatch_locked(
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

seed_lookup_result runtime::run_seed_anchor_roundtrip(
    const gpu_seed_inputs& inputs,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::int32_t max_occ)
{
    std::lock_guard<std::mutex> guard(mu_);
    seed_lookup_result result;
    result.anchor_order_audit = audit_seed_anchor_canonical_order({}, 0u);
    if (!features_.available) throw vulkan_runtime_error("run_seed_anchor_roundtrip on unavailable device");
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw vulkan_runtime_error("gpu seed missing resident index buffers");
    }
    const std::uint32_t mc = inputs.minimizer_count;
    gpu_arena& arena = reusable_gpu_arena();
    if (mc == 0u || max_occ < 0) {
        if (!arena.release_idle_capacity()) {
            throw vulkan_runtime_error(
                "gpu seed idle arena release found live slices");
        }
        return result;
    }
    const std::uint32_t num_blocks = bioseq::common::ceil_div(mc, k_prefix_scan_block_size);
    const std::uint64_t min_words_bytes = inputs.device_minimizer_words == nullptr
        ? std::max<std::uint64_t>(minimizer_device_bytes(inputs.minimizer_word_kind, inputs.minimizer_words32_per_record, mc), 4u)
        : 0u;
    // Each allocate_device call rounds the offset up to k_gpu_buffer_offset_alignment
    // (256), so the five sub-allocations below can need up to 5*256 bytes of
    // alignment padding on top of their byte footprint. Reserve one alignment
    // unit per boundary plus initial cursor slack so cold and reused arenas obey
    // the same capacity invariant.
    std::uint64_t dt = std::uint64_t{mc} * 4u * 2u + std::uint64_t{num_blocks} * 4u + 16u
        + min_words_bytes + 6u * std::uint64_t{k_gpu_buffer_offset_alignment};
    if (!arena.ensure_capacity(dt, 0, 0)) {
        // ensure_capacity never advances a cursor.  Drop any partial/high-water
        // backing before reporting the failure so a retry starts cold and does
        // not inherit memory from a failed request.
        if (!arena.release_idle_capacity()) {
            throw vulkan_runtime_error(
                "gpu seed failed grow left live arena slices");
        }
        throw vulkan_runtime_error("gpu seed arena grow failed");
    }
    const gpu_arena_mark mark = arena.mark();
    struct seed_arena_lifetime {
        gpu_arena& arena;
        gpu_arena_mark mark;
        bool released = false;

        ~seed_arena_lifetime() noexcept {
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
                throw vulkan_runtime_error(
                    "gpu seed idle arena release found live slices");
            }
            released = true;
        }
    } seed_arena_lifetime{arena, mark};

    const gpu_arena_slice counts_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const gpu_arena_slice prefix_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const gpu_arena_slice blocks_s = arena.allocate_device(std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const gpu_arena_slice qrec_s = arena.allocate_device(16u, 16u);
    gpu_arena_slice minw_s{};
    if (inputs.device_minimizer_words == nullptr) minw_s = arena.allocate_device(min_words_bytes, 16u);
    if (!counts_s.is_valid() || !prefix_s.is_valid() || !blocks_s.is_valid() || !qrec_s.is_valid()
        || (inputs.device_minimizer_words == nullptr && !minw_s.is_valid())) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed arena oversubscribed");
    }
    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    const VkBuffer min_words_buffer = inputs.device_minimizer_words != nullptr
        ? inputs.device_minimizer_words->buffer_handle
        : minw_s.buffer->buffer_handle;
    const std::uint64_t min_words_offset = inputs.device_minimizer_words != nullptr ? 0u : minw_s.offset;
    const std::uint64_t min_words_range = inputs.device_minimizer_words != nullptr ? VK_WHOLE_SIZE : minw_s.size;
    if (inputs.device_minimizer_words == nullptr) {
        std::memset(base + minw_s.offset, 0, static_cast<std::size_t>(minw_s.size));
        if (!inputs.host_minimizer_bytes.empty()) {
            std::memcpy(base + minw_s.offset, inputs.host_minimizer_bytes.data(), inputs.host_minimizer_bytes.size());
        }
    }
    const host_query_record qrec{0u, mc, static_cast<std::uint32_t>(query_len), 0u};
    std::memcpy(base + qrec_s.offset, &qrec, sizeof(qrec));

    const compute_pipeline count_pipe = get_compute_pipeline_locked(shader_kind::seed_count, 128u, 1u);
    const compute_pipeline emit_pipe = get_compute_pipeline_locked(shader_kind::seed_emit, 128u, 1u);
    const compute_pipeline scanb = get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scana = get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);

    const std::uint32_t target_offset_lo =
        static_cast<std::uint32_t>(inputs.target_position_offset & 0xffffffffull);
    const std::uint32_t target_offset_hi =
        static_cast<std::uint32_t>(inputs.target_position_offset >> 32);
    const gpu_anchor_sort_key_plan anchor_sort_plan =
        make_anchor_sort_key_plan(inputs.target_len, inputs.target_position_offset);
    seed_push_constants cpush{inputs.minimizer_words32_per_record, inputs.minimizer_word_kind,
        inputs.hash_shift, inputs.pos_bits, kmer_size, inputs.slot_mask,
        static_cast<std::uint32_t>(max_occ), 0u, 1u, inputs.bucket_count,
        target_offset_lo, target_offset_hi, anchor_sort_plan.target_ref_bits};
    const std::uint32_t seed_groups = std::max<std::uint32_t>(
        1u,
        std::min(bioseq::common::ceil_div(mc, 128u), features_.max_workgroup_count_x));

    // --- Submit 1: seed count + prefix scan ---
    {
        static const auto* _rd = bioseq::instrumentation::register_static_region("seed_count");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer_handle, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer_handle, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer_handle, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},  // anchors (unused in count)
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},  // anchor_keys (unused in count)
        }};
        const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {blocks_s.buffer->buffer_handle, blocks_s.offset, blocks_s.size},
        }};
        scan_push_constants s0{mc, k_prefix_scan_block_size, num_blocks, 0u};
        scan_push_constants s1{mc, k_prefix_scan_block_size, num_blocks, 1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed1)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed1)");
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, count_pipe, seed_infos, &cpush, sizeof(cpush), seed_groups);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, scanb, scan_infos, &s0, sizeof(s0), num_blocks);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, scana, scan_infos, &s0, sizeof(s0), 1u);
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, scana, scan_infos, &s1, sizeof(s1), bioseq::common::ceil_div(mc, 128u));
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
        throw vulkan_runtime_error("gpu seed anchor count overflow");
    }

    // --- Submit 2: seed emit + device anchor sort by ref_bits ---
    // The exact anchor count is only known after the count submit, so the anchor
    // and sort-scratch buffers are sized exactly here as standalone device
    // buffers (the count/prefix arena slices from submit 1 must stay live, so the
    // arena is never grown mid-op). They are wrapped as offset-0 gpu_arena_slice
    // for the shared dispatch/sort recorders.
    const std::uint64_t anchor_u64 = total * 3u;          // 24-byte anchors as 3 u64
    const workgroup_plan awg = plan_workgroups(total, features_.max_workgroup_count_x);
    const std::uint64_t exact_reserve =
        seed_anchor_reserved_bytes(total, awg.num_workgroups);
    auto exact_permit =
        bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(exact_reserve);
    if (!exact_permit) {
        auto& admission =
            bioseq::parallelism::memory_admission_controller::instance();
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error(fmt::format(
            "gpu seed exact anchor memory admission failed for {} bytes "
            "(anchors={} workgroups={} reserved={} allocatable={})",
            exact_reserve,
            total,
            awg.num_workgroups,
            admission.reserved_bytes(),
            admission.allocatable_after_reservations()));
    }
    buffer anchors_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer akeys_buf = create_device_buffer(total * 8u, k_compute_buffer_usage);
    buffer anchors_scr_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer akeys_scr_buf = create_device_buffer(total * 8u, k_compute_buffer_usage);
    buffer ahist_buf = create_device_buffer(std::uint64_t{awg.num_workgroups} * 256u * 4u, k_compute_buffer_usage);
    if (anchors_buf.mapped == nullptr) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed non-unified memory");
    }
    const gpu_arena_slice anchors_s{&anchors_buf, 0, anchors_buf.bytes};
    const gpu_arena_slice akeys_s{&akeys_buf, 0, akeys_buf.bytes};
    const gpu_arena_slice anchors_scr_s{&anchors_scr_buf, 0, anchors_scr_buf.bytes};
    const gpu_arena_slice akeys_scr_s{&akeys_scr_buf, 0, akeys_scr_buf.bytes};
    const gpu_arena_slice ahist_s{&ahist_buf, 0, ahist_buf.bytes};

    bool sorted_in_scratch = false;
    {
        static const auto* _rd = bioseq::instrumentation::register_static_region("seed_emit");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
        seed_push_constants epush = cpush; epush.emit_mode = 1u;
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer_handle, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer_handle, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer_handle, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {anchors_s.buffer->buffer_handle, anchors_s.offset, anchors_s.size},
            {akeys_s.buffer->buffer_handle, akeys_s.offset, akeys_s.size},
        }};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed2)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed2)");
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, emit_pipe, seed_infos, &epush, sizeof(epush), seed_groups);
        record_compute_barrier(command_buffer_);
        gpu_sort_plan sort_plan{total, 8u, 24u, 8u, anchor_sort_plan.pass_count};
        record_radix_sort_locked(command_buffer_, akeys_s, anchors_s, akeys_scr_s, anchors_scr_s, ahist_s,
                                 sort_plan, awg.num_workgroups, awg.elems_per_wg, 3u, sorted_in_scratch);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed2)");
    }
    const gpu_arena_slice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
    result.count = total;
    result.host_bytes.resize(static_cast<std::size_t>(total * 24u));
    std::memcpy(result.host_bytes.data(),
                static_cast<const std::byte*>(sorted_anchors.buffer->mapped) + sorted_anchors.offset,
                static_cast<std::size_t>(total * 24u));
    const auto tie_normalization = normalize_seed_anchor_equal_ref_ties(
        std::span<std::byte>{
            result.host_bytes.data(), result.host_bytes.size()},
        result.count);
    if (!tie_normalization.gpu_ref_order_checked
        || !tie_normalization.gpu_ref_order_accepted) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error(
            "gpu seed GPU result is not ordered by Anchor ref_bits");
    }
    result.canonical_tie_normalization_runs =
        tie_normalization.canonical_tie_run_count;
    result.canonical_tie_normalization_anchors =
        tie_normalization.canonical_tie_anchor_count;
    result.anchor_order_audit = audit_seed_anchor_canonical_order(
        std::span<const std::byte>{
            result.host_bytes.data(), result.host_bytes.size()},
        result.count);
    if (!result.anchor_order_audit.canonical_order_checked
        || !result.anchor_order_audit.canonical_order_accepted
        || result.anchor_order_audit.anchor_count != result.count) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error(
            "gpu seed result is not in canonical Anchor order");
    }
    result.sort_stats.radix_sort_operations = 1u;
    result.sort_stats.radix_anchor_count = total;
    result.sort_stats.radix_passes = anchor_sort_plan.pass_count;
    result.sort_stats.radix_histogram_dispatches =
        anchor_sort_plan.pass_count;
    result.sort_stats.radix_scan_dispatches = anchor_sort_plan.pass_count;
    result.sort_stats.radix_scatter_dispatches =
        anchor_sort_plan.pass_count;

    seed_arena_lifetime.release_or_throw();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

} // namespace bioseq::gpu::vulkan

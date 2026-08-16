/**
 * @file core.cppm
 * @brief Defines Vulkan device, buffer, submission, and shader-provider contracts.
 */
module;

#include <cstddef>
#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:receipts.core;

import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.gpu.vulkan.abi;
import bioseq.prefilter.types;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.instrumentation;
import std;

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
/// @return Current fault, gate, and submission counters.
[[nodiscard]] vulkan_submission_test_snapshot capture_vulkan_submission_test_snapshot() noexcept;
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
/// @param[in] kind Rank-prefilter shader identity.
/// @param[in] push_mode Shader push mode encoded in the recorded command.
/// @return The matching replay slot, or no value for an unsupported route.
[[nodiscard]] constexpr std::optional<std::size_t>
rank_prefilter_replay_index(shader_kind kind, std::uint32_t push_mode) noexcept {
    if (kind == shader_kind::sneaky_snake_rank_batch) {
        if (push_mode == 1u)
            return 0u;
        if (push_mode == 8u)
            return 1u;
        return std::nullopt;
    }
    if (push_mode != 0u)
        return std::nullopt;
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

    constexpr bool operator==(const gatekeeper_rank_batch_route&) const noexcept = default;
};

/// @brief Select a specialized GateKeeper shader only for a homogeneous, word-aligned
/// production batch. Every other shape deliberately retains the generic key.
/// @param[in] jobs Borrowed prefilter jobs whose geometry selects the route.
/// @return The specialized or generic GateKeeper route.
[[nodiscard]] inline gatekeeper_rank_batch_route resolve_gatekeeper_rank_batch_route(
    std::span<const bioseq::prefilter::prefilter_job> jobs) noexcept {
    if (jobs.empty())
        return {};

    const std::uint32_t length = jobs.front().query_length;
    const std::uint32_t max_edits = jobs.front().max_edits;
    bool homogeneous_aligned_production = jobs.front().target_length == length &&
                                          (length == 32u || length == 64u) &&
                                          (max_edits == 2u || max_edits == 4u);
    for (const auto& job : jobs) {
        homogeneous_aligned_production =
            homogeneous_aligned_production && job.query_length == length &&
            job.target_length == length && job.max_edits == max_edits &&
            (job.query_offset & 3u) == 0u && (job.target_offset & 3u) == 0u;
    }
    if (!homogeneous_aligned_production)
        return {};

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
/// @param[in] features Immutable Vulkan device feature snapshot.
/// @return A fixed-width canonical driver identity string.
[[nodiscard]] inline std::array<char, 128>
canonical_vulkan_driver_identity(const vulkan_feature_set& features) noexcept {
    std::array<char, 128> result{};
    constexpr char hex[] = "0123456789abcdef";
    std::size_t cursor = 0u;
    const auto append_literal = [&result, &cursor](std::string_view value) {
        if (cursor > result.size() - value.size() - 1u)
            return false;
        for (const char ch : value)
            result[cursor++] = ch;
        return true;
    };
    const auto append_u32 = [&result, &cursor](std::uint32_t value) {
        constexpr char digits[] = "0123456789abcdef";
        if (cursor > result.size() - 9u)
            return false;
        for (std::uint32_t shift = 28u;; shift -= 4u) {
            result[cursor++] = digits[(value >> shift) & 0x0fu];
            if (shift == 0u)
                break;
        }
        return true;
    };
    if (!append_literal("api=") || !append_u32(features.api_version) ||
        !append_literal(";driver=") || !append_u32(features.driver_version) ||
        !append_literal(";vendor=") || !append_u32(features.vendor_id) ||
        !append_literal(";device=") || !append_u32(features.device_id) ||
        !append_literal(";pcuuid=")) {
        return {};
    }
    for (const std::uint8_t byte : features.pipeline_cache_uuid) {
        if (cursor > result.size() - 3u)
            return {};
        result[cursor++] = hex[byte >> 4u];
        result[cursor++] = hex[byte & 0x0fu];
    }
    result[cursor] = '\0';
    return result;
}

/// @brief RAII owner for one (VkBuffer, VkDeviceMemory) pair. Move-only. Destruction
/// routes through `runtime::destroy_buffer` (the single cleanup path).
struct buffer {
    /// @brief Creates an empty buffer owner.
    buffer() = default;

    /// @brief Adopts one Vulkan buffer allocation and its accounting state.
    /// @param[in] owner Runtime responsible for releasing the allocation.
    /// @param[in] buffer_handle Vulkan buffer handle.
    /// @param[in] memory Vulkan device-memory handle backing the buffer.
    /// @param[in] bytes Logical buffer extent.
    /// @param[in] allocation_bytes Allocator-reported memory extent.
    /// @param[in] memory_properties Vulkan properties of the selected memory type.
    /// @param[in] process_footprint_reflected Whether process accounting includes the allocation.
    /// @param[in] mapped Persistent host mapping, or `nullptr` for device-only memory.
    buffer(runtime* owner,
           VkBuffer buffer_handle,
           VkDeviceMemory memory,
           std::uint64_t bytes,
           std::uint64_t allocation_bytes,
           VkMemoryPropertyFlags memory_properties,
           bool process_footprint_reflected,
           void* mapped) noexcept
        : owner(owner), buffer_handle(buffer_handle), memory(memory), bytes(bytes),
          allocation_bytes(allocation_bytes), memory_properties(memory_properties),
          process_footprint_reflected(process_footprint_reflected), mapped(mapped) {}

    /// @brief Prevents two owners from releasing the same Vulkan allocation.
    buffer(const buffer&) = delete;

    /// @brief Prevents copy assignment of Vulkan allocation ownership.
    buffer& operator=(const buffer&) = delete;

    /// @brief Transfers Vulkan allocation ownership from another buffer.
    /// @param[in,out] other Buffer whose handles and accounting state are consumed.
    buffer(buffer&& other) noexcept {
        move_from(other);
    }

    /// @brief Releases the current allocation and transfers another buffer into this owner.
    /// @param[in,out] other Buffer whose handles and accounting state are consumed.
    /// @return This buffer owner.
    buffer& operator=(buffer&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    /// @brief Releases the owned Vulkan allocation through the runtime owner.
    ~buffer() {
        reset();
    }

    /// @brief Releases any owned Vulkan handles and clears their accounting state.
    void reset() noexcept;

    /// @brief Checks whether this owner contains a Vulkan buffer handle.
    /// @return Whether `buffer_handle` is non-null.
    [[nodiscard]] bool is_valid() const noexcept {
        return buffer_handle != VK_NULL_HANDLE;
    }

    /// @brief Checks whether host accesses need neither explicit mapping nor cache maintenance.
    /// @return Whether the memory is both host-visible and host-coherent.
    [[nodiscard]] bool is_host_visible_coherent() const noexcept {
        constexpr VkMemoryPropertyFlags required =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
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
    /// @brief Moves every handle and accounting field, then clears the source.
    /// @param[in,out] other Buffer state to transfer into this owner.
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
/// @param[in] handle Vulkan handle to encode without dereferencing it.
/// @return Stable unsigned handle bits.
template <class p_handle_type>
[[nodiscard]] inline std::uint64_t vulkan_handle_identity(const p_handle_type handle) noexcept {
    static_assert(sizeof(p_handle_type) <= sizeof(std::uint64_t));
    std::uint64_t identity = 0u;
    std::memcpy(&identity, &handle, sizeof(handle));
    return identity;
}

/// @brief Immutable snapshot of one runtime-owned allocation.  The object address is
/// insufficient because buffer is move-assignable. Operation receipt therefore
/// binds its owner, Vulkan allocation handles, and physical/logical sizes too.
struct runtime_buffer_identity {
    const buffer* object = nullptr;
    const runtime* owner = nullptr;
    std::uint64_t buffer_handle = 0u;
    std::uint64_t memory_handle = 0u;
    std::uint64_t bytes = 0u;
    std::uint64_t allocation_bytes = 0u;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return object != nullptr && owner != nullptr && buffer_handle != 0u &&
               memory_handle != 0u && bytes != 0u && allocation_bytes >= bytes;
    }
    constexpr bool operator==(const runtime_buffer_identity&) const noexcept = default;
};

/// @brief Captures the allocation identity required by operation receipts.
/// @param[in] allocation Runtime-owned allocation to capture.
/// @return The allocation identity, or an empty identity for null input.
[[nodiscard]] inline runtime_buffer_identity
make_runtime_buffer_identity(const buffer* allocation) noexcept {
    if (allocation == nullptr)
        return {};
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
        VkDevice device,
        const shader_variant_key& key,
        PFN_vkCreateShaderModule create_shader_module);
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
/// @param[in] provider Optional process-owned provider. Null removes the provider.
void install_runtime_shader_provider(runtime_shader_provider* provider) noexcept {
    runtime_shader_provider_slot() = provider;
}

/// @brief Validates the canonical lowercase hexadecimal runtime-cache digest.
/// @param[in] digest Candidate cache digest.
/// @return True only for a nonzero 16-character lowercase hexadecimal digest.
[[nodiscard]] inline bool is_runtime_cache_digest_valid(std::string_view digest) noexcept {
    return digest.size() == 16u &&
           std::all_of(digest.begin(),
                       digest.end(),
                       [](char value) {
                           return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                       }) &&
           std::any_of(digest.begin(), digest.end(), [](char value) { return value != '0'; });
}

} // namespace bioseq::gpu::vulkan

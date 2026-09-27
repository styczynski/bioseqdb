/**
 * @file core.cppm
 * @brief Defines Vulkan buffers, device properties, and shared runtime result types.
 */
module;

#include <cstddef>
#include <cstdint>
#include <libassert/assert.hpp>
#include <new>

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
/** @brief Failures that tests can inject after a real GPU submission
 * This API is compiled only with BIOSEQ_TEST_INDEXES. */
enum class vulkan_submission_test_fault : std::uint8_t {
    none,                        ///< No fault is armed.
    cancellation_after_submit,   ///< Report cancellation after the submitted work finishes.
    upload_failure_after_submit, ///< Report an spload failure after the submitted work finishes.
};

/// @brief Test counters and wait states for an injected submission failure.
struct vulkan_submission_test_snapshot {
    vulkan_submission_test_fault fault{vulkan_submission_test_fault::none}; ///< Armed fault kind.
    std::uint64_t upload_latches = 0u;   ///< Uploads observed while a test fault was armed.
    std::uint64_t submissions = 0u;      ///< Submissions observed while a test fault was armed.
    std::uint64_t fence_drains = 0u;     ///< Completed fence waits observed by the test.
    std::uint64_t delivered_faults = 0u; ///< Injected failures reported to callers.
    bool waiting_after_submit = false;   ///< Whether the  test has paused after queue submission.
    bool waiting_after_drain = false;    ///< Whether the test has paused after the fence wait.
    bool cancellation_requested = false; ///< Whether cancellation was requested.
    bool upload_failure_latched = false; ///< Whether the upload reached the point where its failure is recorded.
};

/// @brief Resets test counters and, arms a failure for the next matching submission.
void arm_vulkan_submission_test_seam(vulkan_submission_test_fault fault) noexcept;
/// @brief Lets the paused submission finish waiting for its fence.
void release_vulkan_submission_test_fence() noexcept;
/// @brief Lets the test report its failure after the fence wait.
void release_vulkan_submission_test_delivery() noexcept;
/// @brief Clears the injected failure and counters, releasing any test waits.
void reset_vulkan_submission_test_seam() noexcept;
/// @brief Reads the current test fault, wait states, and counters.
[[nodiscard]] vulkan_submission_test_snapshot capture_vulkan_submission_test_snapshot() noexcept;
#endif

/// @brief Alignment results and packed CIGAR operations copied  from the GPU.
struct alignment_cigar_batch_result {
    alignment_cigar_batch_result() = default;
    alignment_cigar_batch_result(const alignment_cigar_batch_result&) = delete;
    alignment_cigar_batch_result& operator=(const alignment_cigar_batch_result&) = delete;
    alignment_cigar_batch_result(alignment_cigar_batch_result&& other) noexcept
        : capacity_permit(std::move(other.capacity_permit)), results(std::move(other.results)),
          packed_cigar(std::move(other.packed_cigar)),
          committed_submission_count(
              std::exchange(other.committed_submission_count, std::uint64_t{0})) {}
    alignment_cigar_batch_result& operator=(alignment_cigar_batch_result&& other) noexcept {
        if (this != &other) {
            // Free both vectors before returning their memory reservation.
            std::vector<gpu_alignment_cigar_result_wire>{}.swap(results);
            std::vector<std::uint32_t>{}.swap(packed_cigar);
            capacity_permit = {};
            capacity_permit = std::move(other.capacity_permit);
            results = std::move(other.results);
            packed_cigar = std::move(other.packed_cigar);
            committed_submission_count =
                std::exchange(other.committed_submission_count, std::uint64_t{0});
        }
        return *this;
    }

    // Declared first so the vectors are destroyed before their memory
    // reservation is returned.
    bioseq::parallelism::memory_permit capacity_permit{};
    std::vector<gpu_alignment_cigar_result_wire> results{}; ///< Alignment result for each job.
    std::vector<std::uint32_t> packed_cigar{};     ///< Packed CIGAR operations from all jobs.
    std::uint64_t committed_submission_count = 0u; ///< Completed GPU submissions used to produce these results.
};

static_assert(!std::is_copy_constructible_v<alignment_cigar_batch_result>);
static_assert(std::is_nothrow_move_constructible_v<alignment_cigar_batch_result>);

/// @brief Mapping-quality results copied from the GPU after alignment.
struct gpu_mapq_batch_result {
    gpu_mapq_batch_result() = default;
    gpu_mapq_batch_result(const gpu_mapq_batch_result&) = delete;
    gpu_mapq_batch_result& operator=(const gpu_mapq_batch_result&) = delete;
    gpu_mapq_batch_result(gpu_mapq_batch_result&& other) noexcept
        : capacity_permit(std::move(other.capacity_permit)), headers(std::move(other.headers)),
          candidates(std::move(other.candidates)), results(std::move(other.results)),
          committed_submission_count(
              std::exchange(other.committed_submission_count, std::uint64_t{0})) {}
    gpu_mapq_batch_result& operator=(gpu_mapq_batch_result&& other) noexcept {
        if (this != &other) {
            // Free the vectors before returning their hemory reservation.
            std::vector<gpu_chain_candidate_header_wire>{}.swap(headers);
            std::vector<gpu_chain_candidate_wire>{}.swap(candidates);
            std::vector<gpu_mapq_result_wire>{}.swap(results);
            capacity_permit = {};
            capacity_permit = std::move(other.capacity_permit);
            headers = std::move(other.headers);
            candidates = std::move(other.candidates);
            results = std::move(other.results);
            committed_submission_count =
                std::exchange(other.committed_submission_count, std::uint64_t{0});
        }
        return *this;
    }

    // Declared first so the vectors are destroyed before their memory reservation
    bioseq::parallelism::memory_permit capacity_permit{};
    std::vector<gpu_chain_candidate_header_wire> headers{};
    std::vector<gpu_chain_candidate_wire> candidates{};
    std::vector<gpu_mapq_result_wire> results{};
    std::uint64_t committed_submission_count = 0u;
};

static_assert(!std::is_copy_constructible_v<gpu_mapq_batch_result>);
static_assert(std::is_nothrow_move_constructible_v<gpu_mapq_batch_result>);
static_assert(std::is_nothrow_move_assignable_v<gpu_mapq_batch_result>);

/// @brief Device properties collected during runtime initialization.
/// A false available  flag means a selected Vulkan operation must report failure.
struct vulkan_feature_set {
    bool available = false;           ///< Whether a usable Vulkan device was found.
    bool shader_int64 = false;        ///< Whether 64-bit integer shaders are supported.
    bool scalar_block_layout = false; ///< Whether scalar block layout is supported.
    bool timeline_semaphore = false;  ///< Whether timeline semaphores are supported.
    bool software_device = false;     ///< Whether Vulkan reports the selected device as a CPU implementation.
    std::uint32_t device_type = 0;    ///< Vulkan physical-device type enum value.
    std::uint32_t api_version = 0;    ///< Packed Vulkan API version reported by the device.
    std::uint32_t driver_version = 0; ///< Driver version reported by the device.
    std::uint32_t vendor_id = 0;      ///< Vendor identifier reported by Vulkan.
    std::uint32_t device_id = 0;      ///< Device identifier reported by Vulkan.
    std::uint32_t max_workgroup_invocations = 0; ///< Maximum invocations per workgroup.
    std::uint32_t max_workgroup_size_x = 0;      ///< Maximum local size along X.
    std::uint32_t max_workgroup_size_y = 0;      ///< Maximum local size along Y.
    std::uint32_t max_workgroup_size_z = 0;      ///< Maximum local size along Z.
    std::uint32_t max_workgroup_count_x = 0;     ///< Maximum dispatched workgroups along X.
    std::uint32_t max_shared_memory_bytes = 0;   ///< Maximum shared memory per workgroup in bytes.
    std::uint64_t max_storage_buffer_range = 0;  ///< Largest storage-buffer range a shader can bind, in bytes.
    std::uint64_t min_storage_buffer_offset_alignment =
        1;                                ///< Required storage-buffer offset alignment.
    std::uint64_t device_local_bytes = 0; ///< Size of the largest device-local memory heap in bytes.
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{}; ///< Stable Vulkan device UUID bytes.
    std::array<std::uint8_t, VK_UUID_SIZE>
        pipeline_cache_uuid{}; ///< Pipeline-cache compatibility UUID.
    std::array<char, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE>
        device_name{}; ///< NUL-terminated device name.
};

/// @brief Number of recorded-command slots for the supported rank-prefilter variants.
inline constexpr std::size_t k_rank_prefilter_replay_route_count = 7u;

/// @brief Finds the recorded-command slot for a rank-prefilter, shader and mode.
/// @param[in] kind Shader to run.
/// @param[in] push_mode Mode passed to the shader.
/// @return A slot  index, or no value when that shader and mode are unsupported.
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

/// @brief GateKeeper shader and recorded-command slot chosen for a batch.
struct gatekeeper_rank_batch_route {
    shader_kind kind = shader_kind::gatekeeper_rank_batch; ///< GateKeeper shader to run.
    std::size_t replay_index = 2u; ///< Slot holding the recorded commands for this shader.

    constexpr bool operator==(const gatekeeper_rank_batch_route&) const noexcept = default;
};

/// @brief Chooses a GateKeeper shader suited to the whole batch.
/// Uses a specialized shader when every query and target has the same supported
/// length and edit limit, wth 4-byte-aligned offsets. Other batches use the generic shader.
/// @param[in] jobs Prefilter jobs to inspect.
/// @return Shader and recorded-command slot for the batch.
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

/// @brief Formats device and driver identifiers for reports and cache checks.
/// Uses numeric Vulkan properties and  the pipeline-cache UUID in a fixed buffer,
/// so formatting needs no heap allocation
/// @param[in] features Device properties collected at initialization.
/// @return A null-terminated identifier, or an empty array if it does not fit.
[[nodiscard]] inline std::array<char, 128>
canonical_vulkan_driver_identity(const vulkan_feature_set& features) noexcept {
    std::array<char, 128> result{};
    constexpr char hex[] = "0123456789abcdef";
    std::size_t cursor = 0u;
    const auto append_literal = [&result, &cursor](std::string_view value) {
        // Append text while leaving room for the final null character.
        if (cursor > result.size() - value.size() - 1u)
            return false;
        for (const char ch : value)
            result[cursor++] = ch;
        return true;
    };
    const auto append_u32 = [&result, &cursor](std::uint32_t value) {
        // Write a 32-bit field as eight lowercase hexadecimal digits.
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

/// @brief Owns a Vulkan buffer and its device memory.
/// Moving transfers ownership. Destruction frees the allocation through the runtime.
struct buffer {
    /// @brief Creates an empty buffer owner.
    buffer() = default;

    /// @brief Own an existing Vulkan allocation.
    /// @param[in] owner Runtime that frees it.
    /// @param[in] buffer_handle Vulkan buffer handle.
    /// @param[in] memory Backing  device memory.
    /// @param[in] bytes Buffer size in bytes.
    /// @param[in] allocation_bytes Physical allocation size.
    /// @param[in] memory_properties Vulkan memory-type properties.
    /// @param[in] process_footprint_reflected Whether process memory accounting includes this allocation.
    /// @param[in] mapped Existing host mapping, or nullptr if unmapped.
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

    /// @brief Keeps Vulkan allocation ownership from being copied by assignment.
    buffer& operator=(const buffer&) = delete;

    /// @brief Takes another buffer's allocation, leaving that buffer empty.
    buffer(buffer&& other) noexcept {
        move_from(other);
    }

    /// @brief Frees this allocation and takes another buffer's allocation.
    /// The source is left empty.
    buffer& operator=(buffer&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    /// @brief Frees this allocation through its runtime.
    ~buffer() {
        reset();
    }

    /// @brief Frees the alloration and clears its handles, mapping, and,  sizes.
    void reset() noexcept;

    /// @brief Returns whether this object holds a Vulkan buffer handle.
    [[nodiscard]] bool is_valid() const noexcept {
        return buffer_handle != VK_NULL_HANDLE;
    }

    /// @brief Checks for host-visible, host-coherent memory.
    /// These properties allow host access without explicit cache flushes.
    [[nodiscard]] bool is_host_visible_coherent() const noexcept {
        constexpr VkMemoryPropertyFlags required =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        return (memory_properties & required) == required;
    }

    runtime* owner = nullptr;                    ///< Runtime that releases this allocation.
    VkBuffer buffer_handle = VK_NULL_HANDLE;     ///< Vulkan buffer handle.
    VkDeviceMemory memory = VK_NULL_HANDLE;      ///< Device memory backing the buffer.
    std::uint64_t bytes = 0;                     ///< Size of the Vulkan buffer in bytes.
    std::uint64_t allocation_bytes = 0;          ///< Physical allocation size in bytes.
    VkMemoryPropertyFlags memory_properties = 0; ///< Vulkan memory-type properties.
    bool process_footprint_reflected =
        false;              ///< Whether process accounting includes the allocation.
    void* mapped = nullptr; ///< Host mapping, or null when the allocation is not mapped.

  private:
    /// @brief Transfers allocation details and leaves the source empty.
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

/// @brief Copy a Vulkan handle's bytes into a zero-initialized 64-bit identifier
/// Accepts pointer and integer handle types without dereferencing them.
/// @param[in] handle Handle to identify.
/// @return Copied handle representation.
template <class p_handle_type>
[[nodiscard]] inline std::uint64_t vulkan_handle_identity(const p_handle_type handle) noexcept {
    static_assert(sizeof(p_handle_type) <= sizeof(std::uint64_t));
    std::uint64_t identity = 0u;
    std::memcpy(&identity, &handle, sizeof(handle));
    return identity;
}

/// @brief Records which  allocation a buffer object held for an operation.
/// A move assignment can replace that allocation at the same object address,
/// so checks also compare the Vulkan handles, runtime, and sizes.
struct runtime_buffer_identity {
    const buffer* object = nullptr;      ///< Borrowed buffer object whose identity is captured.
    const runtime* owner = nullptr;      ///< Runtime that owned the allocation when recorded.
    std::uint64_t buffer_handle = 0u;    ///< Encoded Vulkan buffer handle bits.
    std::uint64_t memory_handle = 0u;    ///< Encoded Vulkan device-memory handle bits.
    std::uint64_t bytes = 0u;            ///< Recorded Vulkan buffer size in bytes.
    std::uint64_t allocation_bytes = 0u; ///< Recorded physical allocation size in bytes.

    /// @brief Checks for nonempty handles and a buffer size that fits the allocation.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return object != nullptr && owner != nullptr && buffer_handle != 0u &&
               memory_handle != 0u && bytes != 0u && allocation_bytes >= bytes;
    }
    constexpr bool operator==(const runtime_buffer_identity&) const noexcept = default;
};

/// @brief Records a buffer's allocation details for later comparison.
/// @param[in] allocation Buffer to inspect.
/// @return Its recorded identity, or an empty identity for null input.
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

/// @brief Consecutive anchors that  the GPU chains as one independent group.
struct gpu_chain_range {
    std::uint32_t begin = 0;  ///< Inclusive start index within the chained batch.
    std::uint32_t length = 0; ///< Number of anchors in the group.
};

/// @brief Reports a Vulkan operation faiure.
/// A selected GPU algorithm passes this, error to its caller instead of falling back to CPU work.
class vulkan_runtime_error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// @brief Loads precompiled MoltenVK shaders supplied by the supervisor.
/// The runtime uses this provider when installed, so the backend worker can
/// create shader modules without running glslang or spirv-cross.
struct runtime_shader_provider {
    /// @brief Prepares shader files before the runtime initializes its device.
    using prepare_function = std::expected<void, std::string> (*)();
    /// @brief Loads a precompiled shader module for the requested variant.
    using load_function = std::expected<shader_module_load, std::string> (*)(
        VkDevice device,
        const shader_variant_key& key,
        PFN_vkCreateShaderModule create_shader_module);
    prepare_function prepare_runtime =
        nullptr;                         ///< Optional prepare hook installed by the supervisor.
    load_function load_module = nullptr; ///< Optional module loader installed by the supervisor.
};

/// @brief Returns the process-wide shader-provider pointer for reading or replacement.
runtime_shader_provider*& runtime_shader_provider_slot() noexcept {
    static runtime_shader_provider* provider = nullptr;
    return provider;
}

/// @brief Sets the  process-wide shader provider. Pass null to remove it
void install_runtime_shader_provider(runtime_shader_provider* provider) noexcept {
    runtime_shader_provider_slot() = provider;
}

/// @brief Checks for a 16-digit lowercase hexadecimal cache digest containing a nonzero digit.
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

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
 * decisions: init failures degrade to `features_.available == false` (CPU
 * fallback), and per-run failures throw `VulkanRuntimeError` which the chaining
 * backend catches before mutating any output slab. Phase A targets unified
 * memory (Apple Silicon / MoltenVK): device-local buffers come back
 * host-visible+coherent, so the round-trip writes/reads them directly without a
 * staging bounce (DOC0304 §4.1).
 */
module;
#include "common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

export module bioseq_gpu.vulkan.runtime;

import mlogging;
import bioseq_parallelism.memory_admission;
import bioseq_gpu.vulkan.shader_compiler;
import bioseq_gpu.vulkan.phase_b_abi;

export namespace bioseq::gpu::vulkan {

class Runtime;

/// Capabilities probed once at init. `available == false` means there is no
/// usable Phase A device and every chaining call must use the CPU backend.
struct VulkanFeatureSet {
    bool available = false;
    bool shader_int64 = false;
    bool scalar_block_layout = false;
    bool timeline_semaphore = false;
    std::uint32_t max_workgroup_invocations = 0;
    std::uint32_t max_workgroup_count_x = 0;
    std::uint32_t max_shared_memory_bytes = 0;
    std::uint64_t device_local_bytes = 0;
};

/// RAII owner for one (VkBuffer, VkDeviceMemory) pair. Move-only; destruction
/// routes through `Runtime::destroy_buffer` (the single cleanup path).
struct Buffer {
    Buffer() = default;
    Buffer(Runtime* owner, VkBuffer buffer, VkDeviceMemory memory,
           std::uint64_t bytes, void* mapped) noexcept
        : owner(owner), buffer(buffer), memory(memory), bytes(bytes), mapped(mapped) {}
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

    Runtime* owner = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint64_t bytes = 0;
    void* mapped = nullptr;

private:
    void move_from(Buffer& other) noexcept {
        owner = other.owner;
        buffer = other.buffer;
        memory = other.memory;
        bytes = other.bytes;
        mapped = other.mapped;
        other.owner = nullptr;
        other.buffer = VK_NULL_HANDLE;
        other.memory = VK_NULL_HANDLE;
        other.bytes = 0;
        other.mapped = nullptr;
    }
};

/// One independent hard chaining range = one dispatched workgroup.
struct GpuChainRange {
    std::uint32_t begin = 0;
    std::uint32_t length = 0;
};

/// Thrown on any per-run Vulkan failure once the device is usable. The chaining
/// backend catches this and falls back to `chain_anchors_scalar`.
class VulkanRuntimeError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Pipeline cache key — only the two workgroup/tile dimensions vary the emitted
/// SPIR-V (shared-array lengths are compile-time). The descriptor-set + pipeline
/// layout are key-independent and created once.
struct ChainingPipelineKey {
    std::uint32_t tile_size = 384;
    std::uint32_t local_size = 128;

    [[nodiscard]] bool operator==(const ChainingPipelineKey&) const = default;
};

/// The 32-byte push-constant block consumed by the tiled-DP shader. Layout is
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
};
static_assert(sizeof(ChainingPushConstants) == 32);
static_assert(offsetof(ChainingPushConstants, anchor_count) == 0);
static_assert(offsetof(ChainingPushConstants, range_count) == 4);
static_assert(offsetof(ChainingPushConstants, max_dist_x) == 8);
static_assert(offsetof(ChainingPushConstants, max_dist_y) == 12);
static_assert(offsetof(ChainingPushConstants, bandwidth) == 16);
static_assert(offsetof(ChainingPushConstants, max_iter) == 20);
static_assert(offsetof(ChainingPushConstants, gap_penalty) == 24);
static_assert(offsetof(ChainingPushConstants, skip_penalty) == 28);

/// Decoded SPIR-V layout decorations, used to validate the direct-anchor ABI
/// (no CPU-side transcode hides a stride/offset mismatch). Returned by
/// `scan_spirv_layout` so both the runtime (pipeline creation) and the unit
/// test (DOC0304 §10.6) can assert the same invariants.
struct SpirvLayoutScan {
    std::vector<std::uint32_t> array_strides;          ///< every ArrayStride decoration value
    std::vector<std::array<std::uint32_t, 4>> structs4; ///< member offsets for every 4-member struct
    std::vector<std::uint32_t> bindings;               ///< every Binding decoration value

    [[nodiscard]] bool has_array_stride(std::uint32_t stride) const noexcept {
        for (std::uint32_t s : array_strides) {
            if (s == stride) return true;
        }
        return false;
    }
    [[nodiscard]] bool has_struct_offsets(const std::array<std::uint32_t, 4>& offsets) const noexcept {
        for (const auto& s : structs4) {
            if (s == offsets) return true;
        }
        return false;
    }
    [[nodiscard]] bool has_binding(std::uint32_t binding) const noexcept {
        for (std::uint32_t b : bindings) {
            if (b == binding) return true;
        }
        return false;
    }
};

/// Scan a compiled SPIR-V module for the decorations Phase A pins: the 24-byte
/// `Anchors` runtime-array stride, the `{0,8,16,20}` `AnchorWire` member
/// offsets, and the four descriptor bindings. Pure function over the word
/// stream — no Vulkan device needed (the §10.6 test calls it directly).
[[nodiscard]] SpirvLayoutScan scan_spirv_layout(std::span<const std::uint32_t> spirv);

/// DOC0307 §4.5 — result of the two-dispatch minimizer sketch roundtrip. The
/// resident `minimizer_words` device buffer is moved into the caller's
/// `VulkanDeviceMinimizerStorage` for seed-lookup reuse; `host_minimizer_bytes`
/// is the packed `count` minimizers downloaded for the arena slab.
struct RuntimeSketchResult {
    Buffer minimizer_words;
    std::vector<std::byte> host_minimizer_bytes;
    std::uint64_t count = 0;
    bool overflow = false;
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
    std::uint32_t position_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
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
    [[nodiscard]] std::uint64_t chaining_dispatch_count() const noexcept {
        return chaining_dispatch_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] Buffer create_host_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    [[nodiscard]] Buffer create_device_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
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
        float skip_penalty);

    // DOC0307 Phase B — two-dispatch minimizer sketch (candidate + exact emit)
    // and seed-hit lookup (lookup + bitonic sort). Both record every H2D copy,
    // dispatch, barrier, and D2H copy under one queue lock, mirroring the Phase A
    // roundtrip discipline. Transient buffers are owned internally; resident
    // index buffers and memory-budget permits are owned by the caller.
    RuntimeSketchResult run_minimizer_sketch_roundtrip(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_minimizers);

    SeedLookupResult run_seed_lookup_roundtrip(
        RuntimeSeedLookupInputs inputs,
        std::span<const std::uint32_t> prefix_counts,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::uint64_t exact_anchor_count);

    /// Largest power-of-two anchor count the GPU bitonic sort can handle in one
    /// workgroup on this device (bounded by workgroup invocations + shared mem).
    /// Larger anchor spans take the CPU sort fallback (DOC0307 §7.2).
    [[nodiscard]] std::uint32_t gpu_anchor_sort_capacity() const noexcept {
        return gpu_anchor_sort_capacity_;
    }

private:
    /// One cached compute pipeline plus its shader module, keyed by tile/local.
    struct PipelineEntry {
        ChainingPipelineKey key{};
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
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
    };

    // Returns by value: the cached entry is trivially copyable (handles only)
    // and the canonical entry lives in `phase_b_pipelines_`. Returning a
    // reference would dangle the moment a later `get_*` push_back reallocates
    // the vector (the descriptor set / pipeline are still owned by the vector).
    PhaseBPipeline get_phase_b_pipeline_locked(
        ShaderKind kind, std::uint32_t local_size, std::uint32_t max_span);
    void bind_phase_b_descriptors_locked(
        const PhaseBPipeline& entry,
        std::span<const VkBuffer> buffers);

    void init();
    void shutdown() noexcept;
    std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    VkPipeline create_pipeline_locked(const ChainingPipelineKey& key);
    void upload_locked(Buffer& transfer_scratch, Buffer& dst, std::span<const std::byte> bytes,
                       VkAccessFlags dst_access);
    void download_locked(Buffer& transfer_scratch, const Buffer& src, std::span<std::byte> bytes);

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
    std::vector<PipelineEntry> pipeline_cache_;
    std::vector<PhaseBPipeline> phase_b_pipelines_;
    std::uint32_t gpu_anchor_sort_capacity_ = 0;

    VulkanFeatureSet features_{};
    std::atomic<std::uint64_t> chaining_dispatch_count_{0};
    std::mutex mu_;
};

[[nodiscard]] inline std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) noexcept {
    constexpr std::uint64_t max_value = (std::numeric_limits<std::uint64_t>::max)();
    if (a > max_value - b) return max_value;
    return a + b;
}

[[nodiscard]] inline std::uint64_t sat_max(std::uint64_t a, std::uint64_t b) noexcept {
    return a > b ? a : b;
}

[[nodiscard]] inline std::uint64_t chaining_reserved_bytes(
    std::uint64_t anchor_count,
    std::uint64_t range_count) noexcept
{
    using bioseq::parallelism::saturating_mul;
    const std::uint64_t anchors = saturating_mul(anchor_count, 24);
    const std::uint64_t scores  = saturating_mul(anchor_count, 4);
    const std::uint64_t preds   = saturating_mul(anchor_count, 8);
    const std::uint64_t ranges  = saturating_mul(range_count, 8);
    const std::uint64_t device_bytes =
        sat_add(sat_add(sat_add(anchors, scores), preds), ranges);
    const std::uint64_t host_dp_bytes =
        sat_add(sat_add(scores, preds), ranges);
    const std::uint64_t transfer_scratch = sat_max(
        sat_max(anchors, preds),
        sat_max(scores, ranges));
    return sat_add(sat_add(device_bytes, host_dp_bytes), transfer_scratch);
}

[[nodiscard]] inline std::uint64_t chaining_transfer_scratch_bytes(
    std::uint64_t anchor_count,
    std::uint64_t range_count) noexcept
{
    using bioseq::parallelism::saturating_mul;
    const std::uint64_t anchors = saturating_mul(anchor_count, 24);
    const std::uint64_t scores  = saturating_mul(anchor_count, 4);
    const std::uint64_t preds   = saturating_mul(anchor_count, 8);
    const std::uint64_t ranges  = saturating_mul(range_count, 8);
    return sat_max(sat_max(anchors, preds), sat_max(scores, ranges));
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

constexpr const char* kPortabilityEnumeration = "VK_KHR_portability_enumeration";
constexpr const char* kGetPhysicalDeviceProperties2 = "VK_KHR_get_physical_device_properties2";
constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";

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

}  // namespace

// ---------------------------------------------------------------------------
// SPIR-V decoration scanner
// ---------------------------------------------------------------------------

SpirvLayoutScan scan_spirv_layout(std::span<const std::uint32_t> spirv) {
    SpirvLayoutScan scan;
    // SPIR-V: 5-word header, then a stream of instructions. Each instruction's
    // first word packs (wordCount << 16) | opcode.
    constexpr std::uint32_t kMagic = 0x07230203u;
    constexpr std::uint16_t kOpDecorate = 71;
    constexpr std::uint16_t kOpMemberDecorate = 72;
    constexpr std::uint32_t kDecorationArrayStride = 6;
    constexpr std::uint32_t kDecorationOffset = 35;
    constexpr std::uint32_t kDecorationBinding = 33;

    if (spirv.size() < 5 || spirv[0] != kMagic) {
        return scan;
    }

    // Per-struct accumulation: member index -> byte offset. Bounded to the
    // 4-member AnchorWire we care about; structs with member index >= 4 are
    // tracked only enough to know they are not the 4-member ABI struct.
    struct StructOffsets {
        std::uint32_t struct_id = 0;
        std::array<std::uint32_t, 4> offsets{};
        std::array<bool, 4> seen{};
        bool overflow = false;  // a member index >= 4 was decorated
    };
    std::vector<StructOffsets> struct_offsets;

    std::size_t i = 5;
    while (i < spirv.size()) {
        const std::uint32_t word = spirv[i];
        const std::uint16_t op = static_cast<std::uint16_t>(word & 0xffffu);
        const std::uint16_t word_count = static_cast<std::uint16_t>(word >> 16);
        if (word_count == 0 || i + word_count > spirv.size()) {
            break;  // malformed; stop scanning
        }

        if (op == kOpDecorate && word_count >= 3) {
            const std::uint32_t decoration = spirv[i + 2];
            if (decoration == kDecorationArrayStride && word_count >= 4) {
                scan.array_strides.push_back(spirv[i + 3]);
            } else if (decoration == kDecorationBinding && word_count >= 4) {
                scan.bindings.push_back(spirv[i + 3]);
            }
        } else if (op == kOpMemberDecorate && word_count >= 4) {
            const std::uint32_t struct_id = spirv[i + 1];
            const std::uint32_t member = spirv[i + 2];
            const std::uint32_t decoration = spirv[i + 3];
            if (decoration == kDecorationOffset && word_count >= 5) {
                const std::uint32_t offset = spirv[i + 4];
                StructOffsets* entry = nullptr;
                for (auto& candidate : struct_offsets) {
                    if (candidate.struct_id == struct_id) {
                        entry = &candidate;
                        break;
                    }
                }
                if (entry == nullptr) {
                    struct_offsets.push_back(StructOffsets{struct_id, {}, {}, false});
                    entry = &struct_offsets.back();
                }
                if (member < 4) {
                    entry->offsets[member] = offset;
                    entry->seen[member] = true;
                } else {
                    entry->overflow = true;
                }
            }
        }
        i += word_count;
    }

    for (const auto& entry : struct_offsets) {
        if (entry.overflow) continue;
        if (entry.seen[0] && entry.seen[1] && entry.seen[2] && entry.seen[3]) {
            scan.structs4.push_back(entry.offsets);
        }
    }
    return scan;
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
    mapped = nullptr;
}

// ---------------------------------------------------------------------------
// Runtime lifecycle
// ---------------------------------------------------------------------------

Runtime::Runtime() {
    try {
        init();
    } catch (const std::exception& e) {
        bioseq_log<DBG>("[vulkan_runtime] init failed; CPU fallback: {}", e.what());
        shutdown();
        features_ = VulkanFeatureSet{};
    } catch (...) {
        bioseq_log<DBG>("[vulkan_runtime] init failed (unknown); CPU fallback");
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

void Runtime::init() {
    // --- Instance ---
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

    check_vk(vkCreateInstance(&inst_info, nullptr, &instance_), "vkCreateInstance");

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

    check_vk(vkCreateDevice(physical_, &device_info, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

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
    features_.max_workgroup_invocations = props.limits.maxComputeWorkGroupInvocations;
    features_.max_workgroup_count_x = props.limits.maxComputeWorkGroupCount[0];
    features_.max_shared_memory_bytes = props.limits.maxComputeSharedMemorySize;

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
    // original index). Anchor spans above this fall back to the CPU radix sort.
    {
        constexpr std::uint32_t per_lane_shared_bytes = 24u + 4u;
        std::uint32_t capacity = 0;
        for (std::uint32_t candidate = 1; candidate <= 1024u; candidate <<= 1) {
            if (candidate > features_.max_workgroup_invocations) break;
            if (static_cast<std::uint64_t>(candidate) * per_lane_shared_bytes
                > features_.max_shared_memory_bytes) {
                break;
            }
            capacity = candidate;
        }
        gpu_anchor_sort_capacity_ = capacity;
    }

    features_.available = true;
    bioseq_log<DBG>(
        "[vulkan_runtime] device ready: {} (maxWGInvocations={}, maxWGCountX={}, "
        "maxSharedMem={}B, scalarBlockLayout={})",
        props.deviceName, features_.max_workgroup_invocations,
        features_.max_workgroup_count_x, features_.max_shared_memory_bytes,
        features_.scalar_block_layout);
}

void Runtime::shutdown() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (auto& entry : pipeline_cache_) {
            if (entry.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, entry.pipeline, nullptr);
            if (entry.module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, entry.module, nullptr);
        }
        pipeline_cache_.clear();
        for (auto& entry : phase_b_pipelines_) {
            if (entry.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, entry.pipeline, nullptr);
            if (entry.module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, entry.module, nullptr);
            if (entry.descriptor_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
            if (entry.pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
            if (entry.set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
        }
        phase_b_pipelines_.clear();
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
    return Buffer{this, buffer, memory, bytes, mapped};
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
    return Buffer{this, buffer, memory, bytes, mapped};
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

VkPipeline Runtime::create_pipeline_locked(const ChainingPipelineKey& key) {
    for (const auto& entry : pipeline_cache_) {
        if (entry.key == key) {
            return entry.pipeline;
        }
    }

    if (key.local_size == 0 || key.local_size > features_.max_workgroup_invocations) {
        throw VulkanRuntimeError(fmt::format(
            "chaining local_size {} exceeds maxComputeWorkGroupInvocations {}",
            key.local_size, features_.max_workgroup_invocations));
    }

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
    VkShaderModule module = VK_NULL_HANDLE;
    check_vk(vkCreateShaderModule(device_, &module_info, nullptr, &module),
             "vkCreateShaderModule");

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = pipeline_layout_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pr = vkCreateComputePipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
    if (pr != VK_SUCCESS) {
        vkDestroyShaderModule(device_, module, nullptr);
        throw VulkanRuntimeError(
            fmt::format("vkCreateComputePipelines: VkResult {}", static_cast<int>(pr)));
    }

    pipeline_cache_.push_back(PipelineEntry{key, module, pipeline});
    bioseq_log<DBG>("[vulkan_runtime] built chaining pipeline tile={} local={}",
                    key.tile_size, key.local_size);
    return pipeline;
}

// ---------------------------------------------------------------------------
// Upload / download helpers (called under mu_, command buffer already open)
// ---------------------------------------------------------------------------

void Runtime::upload_locked(
    Buffer& transfer_scratch, Buffer& dst, std::span<const std::byte> bytes,
    VkAccessFlags dst_access) {
    (void)dst_access;  // host-coherent + queue submit makes host writes visible
    if (bytes.empty()) return;
    if (dst.mapped == nullptr) {
        // Phase A requires host-visible device buffers (unified memory). The
        // scratch param is the future discrete-GPU bounce; reject cleanly here.
        (void)transfer_scratch;
        throw VulkanRuntimeError("device buffer is not host-visible (Phase A needs unified memory)");
    }
    DEBUG_ASSERT(bytes.size() <= dst.bytes, "upload exceeds destination buffer size");
    std::memcpy(dst.mapped, bytes.data(), bytes.size());
}

void Runtime::download_locked(
    Buffer& transfer_scratch, const Buffer& src, std::span<std::byte> bytes) {
    if (bytes.empty()) return;
    if (src.mapped == nullptr) {
        (void)transfer_scratch;
        throw VulkanRuntimeError("device buffer is not host-visible (Phase A needs unified memory)");
    }
    DEBUG_ASSERT(bytes.size() <= src.bytes, "download exceeds source buffer size");
    std::memcpy(bytes.data(), src.mapped, bytes.size());
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
    float skip_penalty) {
    std::lock_guard<std::mutex> guard(mu_);

    if (!features_.available) {
        throw VulkanRuntimeError("run_chaining_tiled_dp_roundtrip on unavailable device");
    }
    if (range_count > features_.max_workgroup_count_x) {
        throw VulkanRuntimeError(fmt::format(
            "range_count {} exceeds maxComputeWorkGroupCount[0] {}",
            range_count, features_.max_workgroup_count_x));
    }
    DEBUG_ASSERT(transfer_scratch.valid(), "transfer scratch buffer must be allocated");

    const VkPipeline pipeline = create_pipeline_locked(key);

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
    vkCmdDispatch(command_buffer_, range_count, 1, 1);

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
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences");

    // D2H: read the propagated scores / predecessors back.
    download_locked(transfer_scratch, best_scores, best_scores_bytes);
    download_locked(transfer_scratch, predecessors, predecessors_bytes);

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// DOC0307 Phase B — seeding roundtrips
// ---------------------------------------------------------------------------

namespace {

/// Push-constant blocks for the Phase B shaders. Each mirrors its GLSL `Params`
/// uniform byte-for-byte (see `push_constant_size` in shader_compiler.cppm).
struct SketchCandidatePush {
    std::uint32_t bp_size;
    std::uint32_t window_size;
    std::uint32_t kmer_size;
    std::uint32_t unit_bits;
    std::uint32_t bits_per_char;
    std::uint32_t letters_per_unit;
    std::uint32_t alphabet_size;
    std::uint32_t alphabet_kind;
    std::uint32_t seq_word_offset;
    std::uint32_t char_mask;
};
static_assert(sizeof(SketchCandidatePush) == 40);

struct SketchEmitPush {
    std::uint32_t bp_size;
    std::uint32_t window_size;
    std::uint32_t kmer_size;
    std::uint32_t pos_bits;
    std::uint32_t hash_bits;
    std::uint32_t hash_shift;
    std::uint32_t word_kind;
    std::uint32_t words32_per_minimizer;
    std::uint32_t span_value;
    std::uint32_t max_minimizers;
};
static_assert(sizeof(SketchEmitPush) == 40);

struct SeedLookupPush {
    std::uint32_t minimizer_count;
    std::uint32_t words32_per_minimizer;
    std::uint32_t word_kind;
    std::uint32_t hash_shift;
    std::uint32_t pos_bits;
    std::uint32_t query_len;
    std::uint32_t kmer_size;
    std::uint32_t slot_mask;
    std::uint32_t exact_anchor_count;
};
static_assert(sizeof(SeedLookupPush) == 36);

constexpr VkBufferUsageFlags kPhaseBBufferUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT;

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

/// `ref_bits()` of a 24-byte Anchor record at `bytes`: ref_position is the
/// leading u64; is_reverse is the u32 at offset 16.
[[nodiscard]] std::uint64_t anchor_ref_bits_at(const std::byte* bytes) noexcept {
    std::uint64_t ref_position = 0;
    std::uint32_t is_reverse = 0;
    std::memcpy(&ref_position, bytes, sizeof(ref_position));
    std::memcpy(&is_reverse, bytes + 16, sizeof(is_reverse));
    return (static_cast<std::uint64_t>(is_reverse != 0u) << 63)
         | (ref_position & ((std::uint64_t{1} << 63) - 1u));
}

/// CPU fallback (DOC0307 §7.2): stable-sort the downloaded 24-byte anchors by
/// `ref_bits()` when the span exceeds the GPU bitonic sort's workgroup capacity.
void cpu_sort_anchor_bytes_by_ref_bits(std::span<std::byte> bytes, std::uint64_t count) {
    constexpr std::size_t kAnchorBytes = 24;
    if (count < 2) return;
    std::vector<std::uint32_t> order(static_cast<std::size_t>(count));
    for (std::uint32_t i = 0; i < count; ++i) order[i] = i;
    const std::byte* base = bytes.data();
    std::stable_sort(order.begin(), order.end(),
        [base](std::uint32_t a, std::uint32_t b) noexcept {
            return anchor_ref_bits_at(base + a * kAnchorBytes)
                 < anchor_ref_bits_at(base + b * kAnchorBytes);
        });
    std::vector<std::byte> sorted(static_cast<std::size_t>(count) * kAnchorBytes);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::memcpy(sorted.data() + i * kAnchorBytes,
                    base + order[i] * kAnchorBytes, kAnchorBytes);
    }
    std::memcpy(bytes.data(), sorted.data(), sorted.size());
}

}  // namespace

Runtime::PhaseBPipeline Runtime::get_phase_b_pipeline_locked(
    ShaderKind kind, std::uint32_t local_size, std::uint32_t max_span)
{
    for (auto& entry : phase_b_pipelines_) {
        if (entry.kind == kind && entry.local_size == local_size
            && entry.max_span == max_span) {
            return entry;
        }
    }
    if (local_size == 0 || local_size > features_.max_workgroup_invocations) {
        throw VulkanRuntimeError(fmt::format(
            "Phase B local_size {} exceeds maxComputeWorkGroupInvocations {}",
            local_size, features_.max_workgroup_invocations));
    }

    const DescriptorLayoutKind layout_kind = descriptor_layout_for(kind);
    const std::uint32_t binding_count = descriptor_binding_count(layout_kind);
    const std::uint32_t push_size = push_constant_size(kind);

    PhaseBPipeline entry;
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
    check_vk(vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr, &entry.set_layout),
             "vkCreateDescriptorSetLayout(phase_b)");

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = push_size;
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &entry.set_layout;
    layout_info.pushConstantRangeCount = push_size > 0 ? 1u : 0u;
    layout_info.pPushConstantRanges = push_size > 0 ? &push_range : nullptr;
    check_vk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &entry.pipeline_layout),
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
    check_vk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &entry.descriptor_pool),
             "vkCreateDescriptorPool(phase_b)");

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = entry.descriptor_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &entry.set_layout;
    check_vk(vkAllocateDescriptorSets(device_, &set_alloc, &entry.descriptor_set),
             "vkAllocateDescriptorSets(phase_b)");

    std::vector<std::uint32_t> spirv;
    try {
        ShaderCompileRequest request{};
        request.kind = kind;
        request.tile_size = 384;
        request.local_size = local_size;
        request.max_span = max_span;
        spirv = compile_shader_spirv(request);
    } catch (const std::exception& compile_error) {
        vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
        vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
        throw VulkanRuntimeError(fmt::format("phase B shader compile failed: {}",
                                             compile_error.what()));
    }

    // Pin the SSBO wire ABI: a candidate-stride / sort-range-stride mismatch
    // would silently corrupt the direct CandidateWire / SortRangeWire reads.
    const SpirvLayoutScan scan = scan_spirv_layout(spirv);
    if (kind == ShaderKind::minimizer_candidates && !scan.has_array_stride(candidate_wire_stride)) {
        vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
        vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
        throw VulkanRuntimeError("SPIR-V Candidate array stride != candidate_wire_stride (32)");
    }
    if (kind == ShaderKind::sort_anchors_bitonic && !scan.has_array_stride(8)) {
        vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
        vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
        throw VulkanRuntimeError("SPIR-V SortRange array stride != 8");
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv.size() * sizeof(std::uint32_t);
    module_info.pCode = spirv.data();
    check_vk(vkCreateShaderModule(device_, &module_info, nullptr, &entry.module),
             "vkCreateShaderModule(phase_b)");

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = entry.module;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = entry.pipeline_layout;
    const VkResult pr = vkCreateComputePipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &entry.pipeline);
    if (pr != VK_SUCCESS) {
        vkDestroyShaderModule(device_, entry.module, nullptr);
        vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
        vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
        throw VulkanRuntimeError(fmt::format(
            "vkCreateComputePipelines(phase_b): VkResult {}", static_cast<int>(pr)));
    }

    bioseq_log<DBG>("[vulkan_runtime] built phase B pipeline kind={} local={} max_span={}",
                    static_cast<int>(kind), local_size, max_span);
    phase_b_pipelines_.push_back(entry);
    return phase_b_pipelines_.back();
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

RuntimeSketchResult Runtime::run_minimizer_sketch_roundtrip(
    std::span<const std::byte> packed_sequence,
    const VulkanRankLut& rank_lut,
    const VulkanSketchParams& params,
    std::uint64_t max_minimizers)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_minimizer_sketch_roundtrip on unavailable device");
    }

    const std::uint32_t bp_size = params.bp_size;
    constexpr std::uint32_t candidate_local = 128u;
    const std::uint64_t candidate_groups =
        (static_cast<std::uint64_t>(bp_size) + candidate_local - 1u) / candidate_local;
    if (candidate_groups > features_.max_workgroup_count_x) {
        throw VulkanRuntimeError("minimizer sketch dispatch exceeds maxComputeWorkGroupCount[0]");
    }

    const PhaseBPipeline cand_pipe =
        get_phase_b_pipeline_locked(ShaderKind::minimizer_candidates, candidate_local, 256u);
    const PhaseBPipeline emit_pipe =
        get_phase_b_pipeline_locked(ShaderKind::minimizer_emit_exact, 1u, 256u);

    const std::uint64_t out_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(params.minimizer_word_kind, params.minimizer_words32, max_minimizers), 4u);
    const std::uint64_t cand_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(bp_size) * sizeof(CandidateWire), 32u);
    const std::uint64_t seq_bytes = std::max<std::uint64_t>(packed_sequence.size(), 4u);

    Buffer scratch = create_host_buffer(4u, kPhaseBBufferUsage);
    Buffer seq_buf = create_device_buffer(seq_bytes, kPhaseBBufferUsage);
    Buffer cand_buf = create_device_buffer(cand_bytes, kPhaseBBufferUsage);
    Buffer lut_buf = create_device_buffer(sizeof(VulkanRankLut), kPhaseBBufferUsage);
    Buffer out_buf = create_device_buffer(out_bytes, kPhaseBBufferUsage);
    Buffer counter_buf = create_device_buffer(8u, kPhaseBBufferUsage);

    upload_locked(scratch, seq_buf, packed_sequence, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(scratch, lut_buf,
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(&rank_lut), sizeof(rank_lut)},
        VK_ACCESS_SHADER_READ_BIT);
    {
        const std::array<std::uint32_t, 2> zero{0u, 0u};
        upload_locked(scratch, counter_buf,
            std::as_bytes(std::span<const std::uint32_t>{zero.data(), zero.size()}),
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    {
        const std::array<VkBuffer, 3> cand_bufs{seq_buf.buffer, cand_buf.buffer, lut_buf.buffer};
        bind_phase_b_descriptors_locked(cand_pipe, cand_bufs);
        const std::array<VkBuffer, 3> emit_bufs{cand_buf.buffer, out_buf.buffer, counter_buf.buffer};
        bind_phase_b_descriptors_locked(emit_pipe, emit_bufs);
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(sketch)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(sketch)");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host_to_shader,
                         0, nullptr, 0, nullptr);

    const SketchCandidatePush cand_push{
        bp_size, params.window_size, params.kmer_size, params.unit_bits,
        params.bits_per_char, params.letters_per_unit, params.alphabet_size,
        params.alphabet_kind, params.seq_word_offset, params.char_mask};
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, cand_pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            cand_pipe.pipeline_layout, 0, 1, &cand_pipe.descriptor_set, 0, nullptr);
    vkCmdPushConstants(command_buffer_, cand_pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(cand_push), &cand_push);
    vkCmdDispatch(command_buffer_, static_cast<std::uint32_t>(candidate_groups), 1, 1);

    VkMemoryBarrier shader_to_shader{};
    shader_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_shader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &shader_to_shader,
                         0, nullptr, 0, nullptr);

    const SketchEmitPush emit_push{
        bp_size, params.window_size, params.kmer_size, params.pos_bits, params.hash_bits,
        params.hash_shift, params.minimizer_word_kind, params.minimizer_words32,
        params.kmer_size, static_cast<std::uint32_t>(max_minimizers)};
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, emit_pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            emit_pipe.pipeline_layout, 0, 1, &emit_pipe.descriptor_set, 0, nullptr);
    vkCmdPushConstants(command_buffer_, emit_pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(emit_push), &emit_push);
    vkCmdDispatch(command_buffer_, 1, 1, 1);

    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &shader_to_host,
                         0, nullptr, 0, nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(sketch)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(sketch)");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(sketch)");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(sketch)");

    std::array<std::uint32_t, 2> counter{0u, 0u};
    download_locked(scratch, counter_buf,
        std::as_writable_bytes(std::span<std::uint32_t>{counter.data(), counter.size()}));

    RuntimeSketchResult result;
    result.overflow = counter[1] != 0u;
    if (!result.overflow) {
        result.count = std::min<std::uint64_t>(counter[0], max_minimizers);
        const std::uint64_t host_bytes =
            minimizer_host_bytes(params.minimizer_word_kind, result.count);
        result.host_minimizer_bytes.resize(static_cast<std::size_t>(host_bytes));
        if (host_bytes > 0) {
            download_locked(scratch, out_buf, std::span<std::byte>{
                result.host_minimizer_bytes.data(), result.host_minimizer_bytes.size()});
        }
        result.minimizer_words = std::move(out_buf);
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(sketch-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

SeedLookupResult Runtime::run_seed_lookup_roundtrip(
    RuntimeSeedLookupInputs inputs,
    std::span<const std::uint32_t> prefix_counts,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::uint64_t exact_anchor_count)
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

    const std::uint64_t anchor_bytes = exact_anchor_count * std::uint64_t{24};
    Buffer anchors_buf = create_device_buffer(
        std::max<std::uint64_t>(anchor_bytes, 24u), kPhaseBBufferUsage);

    const std::uint32_t sort_capacity = gpu_anchor_sort_capacity_;
    const bool gpu_sort = sort_capacity > 0 && exact_anchor_count <= sort_capacity;
    Buffer ranges_buf;
    std::optional<PhaseBPipeline> sort_pipe;
    if (gpu_sort) {
        ranges_buf = create_device_buffer(sizeof(SortRangeWire), kPhaseBBufferUsage);
        const SortRangeWire range{0u, static_cast<std::uint32_t>(exact_anchor_count)};
        upload_locked(scratch, ranges_buf,
            std::span<const std::byte>{reinterpret_cast<const std::byte*>(&range), sizeof(range)},
            VK_ACCESS_SHADER_READ_BIT);
        sort_pipe = get_phase_b_pipeline_locked(
            ShaderKind::sort_anchors_bitonic, sort_capacity, 256u);
    }

    {
        const std::array<VkBuffer, 5> seed_bufs{
            min_words->buffer, inputs.slots->buffer, inputs.positions->buffer,
            prefix_buf.buffer, anchors_buf.buffer};
        bind_phase_b_descriptors_locked(seed_pipe, seed_bufs);
        if (gpu_sort) {
            const std::array<VkBuffer, 2> sort_bufs{anchors_buf.buffer, ranges_buf.buffer};
            bind_phase_b_descriptors_locked(*sort_pipe, sort_bufs);
        }
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
        static_cast<std::uint32_t>(exact_anchor_count)};
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, seed_pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            seed_pipe.pipeline_layout, 0, 1, &seed_pipe.descriptor_set, 0, nullptr);
    vkCmdPushConstants(command_buffer_, seed_pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(seed_push), &seed_push);
    vkCmdDispatch(command_buffer_, static_cast<std::uint32_t>(seed_groups), 1, 1);

    if (gpu_sort) {
        VkMemoryBarrier shader_to_shader{};
        shader_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        shader_to_shader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        shader_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &shader_to_shader,
                             0, nullptr, 0, nullptr);
        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, sort_pipe->pipeline);
        vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                sort_pipe->pipeline_layout, 0, 1, &sort_pipe->descriptor_set, 0, nullptr);
        vkCmdDispatch(command_buffer_, 1, 1, 1);
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
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(seed)");

    SeedLookupResult result;
    result.count = exact_anchor_count;
    result.host_bytes.resize(static_cast<std::size_t>(anchor_bytes));
    download_locked(scratch, anchors_buf, std::span<std::byte>{
        result.host_bytes.data(), result.host_bytes.size()});
    if (!gpu_sort) {
        cpu_sort_anchor_bytes_by_ref_bits(
            std::span<std::byte>{result.host_bytes.data(), result.host_bytes.size()},
            exact_anchor_count);
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

}  // namespace bioseq::gpu::vulkan

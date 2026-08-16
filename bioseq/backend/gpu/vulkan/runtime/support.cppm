/**
 * @file support.cppm
 * @brief Provides module-private Vulkan loading, validation, barriers, and submission support.
 */
module;

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:support;

import :frontend_types;
import :metal_archive;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

#ifdef BIOSEQ_TEST_INDEXES
struct vulkan_submission_test_state {
    std::atomic<std::uint8_t> fault{static_cast<std::uint8_t>(vulkan_submission_test_fault::none)};
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

/// @brief Reads the fault selected for the test-only submission seam.
/// @return Fault visible to the next upload and submission.
[[nodiscard]] vulkan_submission_test_fault active_vulkan_submission_test_fault() noexcept {
    return static_cast<vulkan_submission_test_fault>(
        g_vulkan_submission_test_state.fault.load(std::memory_order_acquire));
}

/// @brief Records whether an armed upload-failure fault reached the upload boundary.
void note_vulkan_submission_test_upload() noexcept {
    const auto fault = active_vulkan_submission_test_fault();
    if (fault == vulkan_submission_test_fault::none) {
        return;
    }
    g_vulkan_submission_test_state.upload_latches.fetch_add(1u, std::memory_order_relaxed);
    if (fault == vulkan_submission_test_fault::upload_failure_after_submit) {
        g_vulkan_submission_test_state.upload_failure_latched.store(1u, std::memory_order_release);
    }
}

/// @brief Holds an armed submission after queue submission and before fence drain.
/// @return Fault that must be delivered after the shared fence is restored.
[[nodiscard]] vulkan_submission_test_fault delay_vulkan_submission_test_after_submit() noexcept {
    const auto fault = active_vulkan_submission_test_fault();
    if (fault == vulkan_submission_test_fault::none) {
        return fault;
    }
    g_vulkan_submission_test_state.submissions.fetch_add(1u, std::memory_order_relaxed);
    g_vulkan_submission_test_state.waiting_after_submit.store(1u, std::memory_order_release);
    std::uint8_t released =
        g_vulkan_submission_test_state.release_gate.load(std::memory_order_acquire);
    while (released == 0u) {
        g_vulkan_submission_test_state.release_gate.wait(0u, std::memory_order_acquire);
        released = g_vulkan_submission_test_state.release_gate.load(std::memory_order_acquire);
    }
    g_vulkan_submission_test_state.waiting_after_submit.store(0u, std::memory_order_release);
    return fault;
}

/// @brief Holds delivery of an armed fault after the fence has been drained.
void delay_vulkan_submission_test_after_drain(vulkan_submission_test_fault fault) noexcept {
    if (fault == vulkan_submission_test_fault::none) {
        return;
    }
    auto& state = g_vulkan_submission_test_state;
    state.fence_drains.fetch_add(1u, std::memory_order_release);
    state.waiting_after_drain.store(1u, std::memory_order_release);
    std::uint8_t released = state.delivery_gate.load(std::memory_order_acquire);
    while (released == 0u) {
        state.delivery_gate.wait(0u, std::memory_order_acquire);
        released = state.delivery_gate.load(std::memory_order_acquire);
    }
    state.waiting_after_drain.store(0u, std::memory_order_release);
}

/// @brief Delivers an armed fault only after queue and fence cleanup has completed.
void deliver_vulkan_submission_test_fault_after_drain(vulkan_submission_test_fault fault) {
    if (fault == vulkan_submission_test_fault::none) {
        return;
    }
    g_vulkan_submission_test_state.delivered_faults.fetch_add(1u, std::memory_order_release);
    g_vulkan_submission_test_state.fault.store(
        static_cast<std::uint8_t>(vulkan_submission_test_fault::none), std::memory_order_release);
    if (fault == vulkan_submission_test_fault::cancellation_after_submit) {
        throw vulkan_runtime_error("injected Vulkan cancellation after fence drain");
    }
    if (g_vulkan_submission_test_state.upload_failure_latched.load(std::memory_order_acquire) !=
        0u) {
        throw vulkan_runtime_error("injected Vulkan upload failure after fence drain");
    }
    throw vulkan_runtime_error("injected Vulkan upload failure was not latched before submit");
}
#endif

/// @brief Throw `vulkan_runtime_error` on any non-success VkResult. A free function
/// rather than a capture-all lambda so it leaves no per-call closure in the hot
/// path and is trivially inlinable.
inline void check_vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw vulkan_runtime_error(fmt::format("{}: VkResult {}", what, static_cast<int>(result)));
    }
}

#define BIOSEQ_VULKAN_RUNTIME_FUNCTIONS(X)                                                         \
    X(vkAllocateCommandBuffers)                                                                    \
    X(vkAllocateDescriptorSets)                                                                    \
    X(vkAllocateMemory)                                                                            \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkBindBufferMemory)                                                                          \
    X(vkCmdBindDescriptorSets)                                                                     \
    X(vkCmdBindPipeline)                                                                           \
    X(vkCmdCopyBuffer)                                                                             \
    X(vkCmdDispatch)                                                                               \
    X(vkCmdPipelineBarrier)                                                                        \
    X(vkCmdPushConstants)                                                                          \
    X(vkCreateBuffer)                                                                              \
    X(vkCreateCommandPool)                                                                         \
    X(vkCreateComputePipelines)                                                                    \
    X(vkCreateDescriptorPool)                                                                      \
    X(vkCreateDescriptorSetLayout)                                                                 \
    X(vkCreateDevice)                                                                              \
    X(vkCreateFence)                                                                               \
    X(vkCreateInstance)                                                                            \
    X(vkCreatePipelineLayout)                                                                      \
    X(vkCreateShaderModule)                                                                        \
    X(vkDestroyBuffer)                                                                             \
    X(vkDestroyCommandPool)                                                                        \
    X(vkDestroyDescriptorPool)                                                                     \
    X(vkDestroyDescriptorSetLayout)                                                                \
    X(vkDestroyDevice)                                                                             \
    X(vkDestroyFence)                                                                              \
    X(vkDestroyInstance)                                                                           \
    X(vkDestroyPipeline)                                                                           \
    X(vkDestroyPipelineLayout)                                                                     \
    X(vkDestroyShaderModule)                                                                       \
    X(vkDeviceWaitIdle)                                                                            \
    X(vkEndCommandBuffer)                                                                          \
    X(vkEnumerateDeviceExtensionProperties)                                                        \
    X(vkEnumerateInstanceExtensionProperties)                                                      \
    X(vkEnumeratePhysicalDevices)                                                                  \
    X(vkFreeMemory)                                                                                \
    X(vkGetBufferMemoryRequirements)                                                               \
    X(vkGetDeviceQueue)                                                                            \
    X(vkGetPhysicalDeviceFeatures)                                                                 \
    X(vkGetPhysicalDeviceFeatures2)                                                                \
    X(vkGetPhysicalDeviceMemoryProperties)                                                         \
    X(vkGetPhysicalDeviceProperties)                                                               \
    X(vkGetPhysicalDeviceProperties2)                                                              \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                    \
    X(vkMapMemory)                                                                                 \
    X(vkQueueSubmit)                                                                               \
    X(vkResetCommandBuffer)                                                                        \
    X(vkResetDescriptorPool)                                                                       \
    X(vkResetFences)                                                                               \
    X(vkUnmapMemory)                                                                               \
    X(vkUpdateDescriptorSets)                                                                      \
    X(vkWaitForFences)

#define BIOSEQ_DECLARE_VULKAN_FUNCTION(name) inline PFN_##name name = nullptr;
BIOSEQ_VULKAN_RUNTIME_FUNCTIONS(BIOSEQ_DECLARE_VULKAN_FUNCTION)
#undef BIOSEQ_DECLARE_VULKAN_FUNCTION

constexpr const char* k_portability_enumeration = "VK_KHR_portability_enumeration";
constexpr const char* k_get_physical_device_properties2 = "VK_KHR_get_physical_device_properties2";
constexpr const char* k_portability_subset = "VK_KHR_portability_subset";
constexpr const char* k_layer_settings = VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;

/// @brief True if `name` appears in a list of VkExtensionProperties.
/// @return `true` when the extension name is present.
[[nodiscard]] bool has_extension(const std::vector<VkExtensionProperties>& available,
                                 const char* name) noexcept {
    for (const auto& ext : available) {
        if (std::strcmp(ext.extensionName, name) == 0)
            return true;
    }
    return false;
}

/// @brief Enumerates loader-visible instance extensions without changing loader state.
/// @return Available instance extensions or an empty list when enumeration fails.
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

/// @brief Enumerates extensions for one physical device.
/// @return Available device extensions or an empty list when enumeration fails.
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
    constexpr std::array candidates = {
        "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json",
    };
    for (const char* path : candidates) {
        if (std::FILE* manifest_file = std::fopen(path, "r")) {
            std::fclose(manifest_file);
            if (::setenv("VK_ICD_FILENAMES", path, 0) == 0) {
                bioseq_log<logging_level::debug>("[vulkan_runtime] using MoltenVK ICD manifest {}",
                                                 path);
            }
            return;
        }
    }
#endif
}

#ifdef BIOSEQ_TEST_INDEXES
void arm_vulkan_submission_test_seam(vulkan_submission_test_fault fault) noexcept {
    auto& state = g_vulkan_submission_test_state;
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
    state.fault.store(static_cast<std::uint8_t>(fault), std::memory_order_release);
}

void release_vulkan_submission_test_fence() noexcept {
    auto& state = g_vulkan_submission_test_state;
    if (active_vulkan_submission_test_fault() ==
        vulkan_submission_test_fault::cancellation_after_submit) {
        state.cancellation_requested.store(1u, std::memory_order_release);
    }
    state.release_gate.store(1u, std::memory_order_release);
    state.release_gate.notify_all();
}

void release_vulkan_submission_test_delivery() noexcept {
    auto& state = g_vulkan_submission_test_state;
    state.delivery_gate.store(1u, std::memory_order_release);
    state.delivery_gate.notify_all();
}

void reset_vulkan_submission_test_seam() noexcept {
    auto& state = g_vulkan_submission_test_state;
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

[[nodiscard]] vulkan_submission_test_snapshot capture_vulkan_submission_test_snapshot() noexcept {
    auto& state = g_vulkan_submission_test_state;
    return {
        .fault = active_vulkan_submission_test_fault(),
        .upload_latches = state.upload_latches.load(std::memory_order_acquire),
        .submissions = state.submissions.load(std::memory_order_acquire),
        .fence_drains = state.fence_drains.load(std::memory_order_acquire),
        .delivered_faults = state.delivered_faults.load(std::memory_order_acquire),
        .waiting_after_submit = state.waiting_after_submit.load(std::memory_order_acquire) != 0u,
        .waiting_after_drain = state.waiting_after_drain.load(std::memory_order_acquire) != 0u,
        .cancellation_requested =
            state.cancellation_requested.load(std::memory_order_acquire) != 0u,
        .upload_failure_latched =
            state.upload_failure_latched.load(std::memory_order_acquire) != 0u,
    };
}
#endif

/// @brief Resolves every Vulkan entry point required by the synchronous runtime.
void load_vulkan_runtime_functions() {
    std::string missing;
#define BIOSEQ_LOAD_VULKAN_FUNCTION(name)                                                          \
    do {                                                                                           \
        if (name == nullptr) {                                                                     \
            name = reinterpret_cast<PFN_##name>(::dlsym(RTLD_DEFAULT, #name));                     \
        }                                                                                          \
        if (name == nullptr) {                                                                     \
            if (!missing.empty())                                                                  \
                missing += ", ";                                                                   \
            missing += #name;                                                                      \
        }                                                                                          \
    } while (false);
    BIOSEQ_VULKAN_RUNTIME_FUNCTIONS(BIOSEQ_LOAD_VULKAN_FUNCTION)
#undef BIOSEQ_LOAD_VULKAN_FUNCTION
    if (!missing.empty()) {
        throw vulkan_runtime_error(
            fmt::format("Vulkan loader is missing required entry points: {}", missing));
    }
}

/// @brief Loads a process-visible Vulkan loader when needed, then resolves its entry points.
void ensure_vulkan_loader_loaded_for_runtime() {
    if (::dlsym(RTLD_DEFAULT, "vkCreateInstance") == nullptr) {
#if defined(__APPLE__)
        constexpr std::array candidates = {
            "@rpath/libvulkan.1.4.341.dylib",
            "@rpath/libvulkan.1.dylib",
            "@rpath/libvulkan.dylib",
            "/opt/homebrew/lib/libvulkan.dylib",
            "/usr/local/lib/libvulkan.dylib",
        };
#else
        constexpr std::array candidates = {
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
                if (!errors.empty())
                    errors += "; ";
                errors += path;
                errors += ": ";
                errors += err;
            }
        }
        if (!loaded && ::dlsym(RTLD_DEFAULT, "vkCreateInstance") == nullptr) {
            throw vulkan_runtime_error(
                fmt::format("Vulkan loader unavailable for backend runtime initialization: {}",
                            errors.empty() ? "no dlopen diagnostics" : errors));
        }
    }
    load_vulkan_runtime_functions();
}

/// @brief Resolves the image that owns a loaded symbol on platforms that expose it.
/// @return Owning image path or an empty string when unavailable.
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

/// @brief Copies host bytes into a mapped device allocation before shader access.
void runtime::upload_locked(buffer& transfer_scratch,
                            buffer& dst,
                            std::span<const std::byte> bytes,
                            VkAccessFlags dst_access) {
    (void)transfer_scratch; // unified-memory path has no bounce allocation
    (void)dst_access;       // host-coherent + queue submit makes host writes visible
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
    DEBUG_ASSERT(bytes.size() <= dst.bytes, "upload exceeds destination buffer size");
    if (bytes.data() != static_cast<const std::byte*>(dst.mapped)) {
        std::memcpy(dst.mapped, bytes.data(), bytes.size());
    }
}

/// @brief Copies a mapped device allocation into caller-owned host bytes.
void runtime::download_locked(buffer& transfer_scratch,
                              const buffer& src,
                              std::span<std::byte> bytes) {
    (void)transfer_scratch; // unified-memory path has no bounce allocation
    if (bytes.empty())
        return;
    if (src.mapped == nullptr) {
        throw vulkan_runtime_error(
            "device buffer is not host-visible (Vulkan chaining needs unified memory)");
    }
    DEBUG_ASSERT(bytes.size() <= src.bytes, "download exceeds source buffer size");
    if (bytes.data() != static_cast<std::byte*>(src.mapped)) {
        std::memcpy(bytes.data(), src.mapped, bytes.size());
    }
}

// ---------------------------------------------------------------------------
// The one hot entry point
// ---------------------------------------------------------------------------

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

constexpr VkBufferUsageFlags k_compute_buffer_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
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

/// @brief Encodes alignment-extension parameters into the shader push layout.
/// @return ABI-pinned push constants for one dispatch.
[[nodiscard]] alignment_extension_push_constants
alignment_extension_push(std::uint32_t job_count,
                         const gpu_alignment_extension_params& params) noexcept {
    return alignment_extension_push_constants{job_count,
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

/// @brief Encodes bounded-CIGAR parameters into the shader push layout.
/// @return ABI-pinned push constants for one dispatch.
[[nodiscard]] alignment_cigar_push_constants
alignment_cigar_push(std::uint32_t job_count, const gpu_alignment_cigar_params& params) noexcept {
    return alignment_cigar_push_constants{job_count,
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

/// @brief Rejects an alignment-extension score outside the shader domain.
void validate_alignment_extension_score(const char* name, std::int32_t value) {
    constexpr std::int32_t k_max_absolute_score = 1'000'000;
    if (value < 0 || value > k_max_absolute_score) {
        throw vulkan_runtime_error(fmt::format(
            "alignment extension {} {} out of range (0..{})", name, value, k_max_absolute_score));
    }
}

/// @brief Checks one symbol range against stream bounds and alphabet width.
void validate_alignment_extension_symbols(std::span<const std::uint32_t> symbols,
                                          std::uint32_t offset,
                                          std::uint32_t length,
                                          std::uint32_t alphabet_size,
                                          const char* label,
                                          std::size_t job_index) {
    const std::uint64_t begin = offset;
    const std::uint64_t end = begin + static_cast<std::uint64_t>(length);
    if (begin > symbols.size() || end > symbols.size()) {
        throw vulkan_runtime_error(
            fmt::format("alignment extension job {} {} range [{}, {}) exceeds symbol count {}",
                        job_index,
                        label,
                        begin,
                        end,
                        symbols.size()));
    }
    for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint32_t symbol = symbols[static_cast<std::size_t>(offset) + i];
        if (symbol >= alphabet_size) {
            throw vulkan_runtime_error(fmt::format("alignment extension job {} {} symbol {} at "
                                                   "local offset {} exceeds alphabet_size {}",
                                                   job_index,
                                                   label,
                                                   symbol,
                                                   i,
                                                   alphabet_size));
        }
    }
}

/// @brief Validates all alignment-extension spans and ABI fields before submission.
void validate_alignment_extension_inputs(std::span<const std::uint32_t> query_symbols,
                                         std::span<const std::uint32_t> target_symbols,
                                         std::span<const gpu_alignment_extension_job_wire> jobs,
                                         const gpu_alignment_extension_params& params) {
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error("alignment extension job_count exceeds uint32");
    }
    if (query_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error(
            "alignment extension query symbol stream exceeds uint32 offsets");
    }
    if (target_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error(
            "alignment extension target symbol stream exceeds uint32 offsets");
    }
    if (params.max_query_len == 0 || params.max_query_len > k_gpu_alignment_extension_max_len) {
        throw vulkan_runtime_error(
            fmt::format("alignment extension max_query_len {} out of range (1..{})",
                        params.max_query_len,
                        k_gpu_alignment_extension_max_len));
    }
    if (params.max_target_len == 0 || params.max_target_len > k_gpu_alignment_extension_max_len) {
        throw vulkan_runtime_error(
            fmt::format("alignment extension max_target_len {} out of range (1..{})",
                        params.max_target_len,
                        k_gpu_alignment_extension_max_len));
    }
    if (params.max_band_width > k_gpu_alignment_extension_max_band_width) {
        throw vulkan_runtime_error(fmt::format("alignment extension max_band_width {} exceeds {}",
                                               params.max_band_width,
                                               k_gpu_alignment_extension_max_band_width));
    }
    if (params.alphabet_size == 0 || params.alphabet_size > 256u) {
        throw vulkan_runtime_error(fmt::format(
            "alignment extension alphabet_size {} out of range (1..256)", params.alphabet_size));
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
            throw vulkan_runtime_error(
                fmt::format("alignment extension job {} query_len {} exceeds max_query_len {}",
                            i,
                            job.query_len,
                            params.max_query_len));
        }
        if (job.target_len > params.max_target_len) {
            throw vulkan_runtime_error(
                fmt::format("alignment extension job {} target_len {} exceeds max_target_len {}",
                            i,
                            job.target_len,
                            params.max_target_len));
        }
        if (job.band_width > params.max_band_width) {
            throw vulkan_runtime_error(
                fmt::format("alignment extension job {} band_width {} exceeds max_band_width {}",
                            i,
                            job.band_width,
                            params.max_band_width));
        }
        if (job.flags != 0 || job.reserved0 != 0 || job.reserved1 != 0) {
            throw vulkan_runtime_error(
                fmt::format("alignment extension job {} flags/reserved must be zero", i));
        }
        validate_alignment_extension_symbols(
            query_symbols, job.query_offset, job.query_len, params.alphabet_size, "query", i);
        validate_alignment_extension_symbols(
            target_symbols, job.target_offset, job.target_len, params.alphabet_size, "target", i);
    }
}

/// @brief Checks shared prefilter stream bounds before replaying a recorded command.
void validate_prefilter_inputs(const char* algorithm,
                               std::span<const std::uint8_t> query_ranks,
                               std::span<const std::uint8_t> target_ranks,
                               std::span<const bioseq::prefilter::prefilter_job> jobs) {
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error(fmt::format("{} job_count exceeds uint32", algorithm));
    }
    if (query_ranks.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error(
            fmt::format("{} query rank stream exceeds uint32 offsets", algorithm));
    }
    if (target_ranks.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw vulkan_runtime_error(
            fmt::format("{} target rank stream exceeds uint32 offsets", algorithm));
    }
}

/// @brief Verifies SneakySnake result status and rejection diagnostics per job.
void validate_sneaky_snake_results(std::span<const bioseq::prefilter::prefilter_job> jobs,
                                   std::span<const bioseq::prefilter::prefilter_result> results,
                                   std::size_t query_rank_count,
                                   std::size_t target_rank_count) {
    using bioseq::prefilter::prefilter_status;
    if (results.size() != jobs.size()) {
        throw vulkan_runtime_error("SneakySnake result count mismatch");
    }
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const bioseq::prefilter::prefilter_job& job = jobs[i];
        const bioseq::prefilter::prefilter_result& result = results[i];
        const bool in_bounds = bioseq::prefilter::prefilter_slice_in_bounds(
                                   query_rank_count, job.query_offset, job.query_length) &&
                               bioseq::prefilter::prefilter_slice_in_bounds(
                                   target_rank_count, job.target_offset, job.target_length);
        if (result.is_accepted > 1u || result.diagnostics != 0u) {
            throw vulkan_runtime_error(
                fmt::format("SneakySnake shader returned malformed flags for job {}", i));
        }

        if (result.status ==
            static_cast<std::uint32_t>(prefilter_status::unsupported_conservative)) {
            if ((in_bounds && job.max_edits <= k_gpu_sneaky_snake_max_edits) ||
                result.lower_bound != 0u || result.is_accepted != 1u) {
                throw vulkan_runtime_error(fmt::format(
                    "SneakySnake shader returned invalid unsupported result for job {}", i));
            }
            continue;
        }

        if (!in_bounds || job.max_edits > k_gpu_sneaky_snake_max_edits) {
            throw vulkan_runtime_error(
                fmt::format("SneakySnake shader failed to retain unsupported job {}", i));
        }
        if (result.status == static_cast<std::uint32_t>(prefilter_status::conservative_unequal)) {
            if (job.query_length == job.target_length) {
                throw vulkan_runtime_error(fmt::format(
                    "SneakySnake shader returned unequal-length status for equal job {}", i));
            }
            const std::uint32_t length_difference = job.query_length > job.target_length
                                                        ? job.query_length - job.target_length
                                                        : job.target_length - job.query_length;
            const std::uint32_t expected_accepted = length_difference <= job.max_edits ? 1u : 0u;
            if (result.lower_bound != length_difference ||
                result.is_accepted != expected_accepted) {
                throw vulkan_runtime_error(fmt::format(
                    "SneakySnake shader returned invalid length-difference result for job {}", i));
            }
            continue;
        }
        if (result.status != static_cast<std::uint32_t>(prefilter_status::success) ||
            job.query_length != job.target_length) {
            throw vulkan_runtime_error(fmt::format(
                "SneakySnake shader returned invalid status {} for job {}", result.status, i));
        }
        const std::uint32_t expected_accepted = result.lower_bound <= job.max_edits ? 1u : 0u;
        if (result.is_accepted != expected_accepted || result.lower_bound > job.max_edits + 1u) {
            throw vulkan_runtime_error(fmt::format(
                "SneakySnake shader returned invalid equal-length result for job {}", i));
        }
    }
}

/// @brief Verifies GateKeeper result status and exact-guard diagnostics per job.
void validate_gatekeeper_results(std::span<const bioseq::prefilter::prefilter_job> jobs,
                                 std::span<const bioseq::prefilter::prefilter_result> results,
                                 std::size_t query_rank_count,
                                 std::size_t target_rank_count) {
    using bioseq::prefilter::gatekeeper_diagnostic_estimate;
    using bioseq::prefilter::is_gatekeeper_exact_guard_activated;
    using bioseq::prefilter::is_gatekeeper_guard_confirmed_rejection;
    using bioseq::prefilter::is_gatekeeper_hamming_exact_rejection;
    using bioseq::prefilter::is_gatekeeper_mask_rejection_candidate;
    using bioseq::prefilter::k_gatekeeper_diagnostic_valid_mask;
    using bioseq::prefilter::prefilter_job;
    using bioseq::prefilter::prefilter_result;
    using bioseq::prefilter::prefilter_status;
    if (results.size() != jobs.size()) {
        throw vulkan_runtime_error("GateKeeper result count mismatch");
    }
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const prefilter_job& job = jobs[i];
        const prefilter_result& result = results[i];
        if (result.is_accepted > 1u ||
            (result.diagnostics & ~k_gatekeeper_diagnostic_valid_mask) != 0u) {
            throw vulkan_runtime_error(
                fmt::format("GateKeeper shader returned malformed flags for job {}", i));
        }

        const std::uint32_t unsupported =
            static_cast<std::uint32_t>(prefilter_status::unsupported_conservative);
        const std::uint32_t unequal =
            static_cast<std::uint32_t>(prefilter_status::conservative_unequal);
        const std::uint32_t success = static_cast<std::uint32_t>(prefilter_status::success);
        const bool in_bounds = bioseq::prefilter::prefilter_slice_in_bounds(
                                   query_rank_count, job.query_offset, job.query_length) &&
                               bioseq::prefilter::prefilter_slice_in_bounds(
                                   target_rank_count, job.target_offset, job.target_length);
        if (!in_bounds) {
            if (result.status != unsupported || result.lower_bound != 0u ||
                result.is_accepted != 1u || result.diagnostics != 0u) {
                throw vulkan_runtime_error(
                    fmt::format("GateKeeper shader did not fail open invalid job {}", i));
            }
            continue;
        }
        if (job.query_length == 0u && job.target_length == 0u) {
            if (result.status != success || result.lower_bound != 0u || result.is_accepted != 1u ||
                result.diagnostics != 0u) {
                throw vulkan_runtime_error(
                    fmt::format("GateKeeper shader returned invalid empty result for job {}", i));
            }
            continue;
        }
        if (job.query_length != job.target_length) {
            const std::uint32_t length_difference = job.query_length > job.target_length
                                                        ? job.query_length - job.target_length
                                                        : job.target_length - job.query_length;
            const std::uint32_t expected_accepted = length_difference <= job.max_edits ? 1u : 0u;
            if (result.status != unequal || result.lower_bound != length_difference ||
                result.is_accepted != expected_accepted || result.diagnostics != 0u) {
                throw vulkan_runtime_error(fmt::format(
                    "GateKeeper shader returned invalid unequal-length result for job {}", i));
            }
            continue;
        }

        const bool supported_geometry = job.query_length <= k_gpu_gatekeeper_max_length &&
                                        job.max_edits <= k_gpu_gatekeeper_max_edits;
        if (!supported_geometry) {
            if (result.status != unsupported || result.lower_bound != 0u ||
                result.is_accepted != 1u || result.diagnostics != 0u) {
                throw vulkan_runtime_error(fmt::format(
                    "GateKeeper shader returned invalid unsupported result for job {}", i));
            }
            continue;
        }

        if (result.status != success) {
            throw vulkan_runtime_error(fmt::format(
                "GateKeeper shader returned invalid status {} for job {}", result.status, i));
        }

        const bool candidate = is_gatekeeper_mask_rejection_candidate(result);
        const bool guard = is_gatekeeper_exact_guard_activated(result);
        const bool confirmed = is_gatekeeper_guard_confirmed_rejection(result);
        const bool hamming = is_gatekeeper_hamming_exact_rejection(result);
        const std::uint32_t estimate = gatekeeper_diagnostic_estimate(result);
        if (candidate != guard || (confirmed && !guard) ||
            (hamming && (candidate || guard || confirmed)) || estimate > job.max_edits + 1u ||
            ((candidate || hamming) && estimate != job.max_edits + 1u) ||
            (!candidate && !hamming && estimate > job.max_edits) ||
            (hamming && job.max_edits != 0u)) {
            throw vulkan_runtime_error(
                fmt::format("GateKeeper shader returned inconsistent diagnostics for job {}", i));
        }
        if (result.is_accepted != 0u) {
            if (result.lower_bound != 0u || confirmed || hamming) {
                throw vulkan_runtime_error(fmt::format(
                    "GateKeeper shader returned invalid accepted result for job {}", i));
            }
        } else if (result.lower_bound != job.max_edits + 1u || (!confirmed && !hamming)) {
            throw vulkan_runtime_error(
                fmt::format("GateKeeper shader returned unconfirmed rejection for job {}", i));
        }
    }
}

/// @brief Rejects a bounded-CIGAR score outside the shader domain.
void validate_alignment_cigar_score(const char* name, std::int32_t value) {
    constexpr std::int32_t k_max_absolute_score = 1'000'000;
    if (value < 0 || value > k_max_absolute_score) {
        throw vulkan_runtime_error(
            fmt::format("alignment CIGAR extension {} {} out of range (0..{})",
                        name,
                        value,
                        k_max_absolute_score));
    }
}

/// @brief Validates bounded-CIGAR streams, traceback spans, and output capacity.
void validate_alignment_cigar_inputs(std::span<const std::uint32_t> query_symbols,
                                     std::span<const std::uint32_t> target_symbols,
                                     std::span<const gpu_alignment_cigar_job_wire> jobs,
                                     const gpu_alignment_cigar_params& params,
                                     std::uint32_t packed_cigar_capacity) {
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
        throw vulkan_runtime_error(
            fmt::format("alignment CIGAR extension alphabet_size {} out of range (1..256)",
                        params.alphabet_size));
    }
    if (params.max_band_width > k_gpu_alignment_extension_max_band_width) {
        throw vulkan_runtime_error(
            fmt::format("alignment CIGAR extension max_band_width {} exceeds {}",
                        params.max_band_width,
                        k_gpu_alignment_extension_max_band_width));
    }
    if (params.tile_query == 0 || params.tile_query > k_gpu_alignment_extension_max_len ||
        params.tile_target == 0 || params.tile_target > k_gpu_alignment_extension_max_len) {
        throw vulkan_runtime_error(
            fmt::format("alignment CIGAR extension tile geometry {}x{} out of range (1..{})",
                        params.tile_query,
                        params.tile_target,
                        k_gpu_alignment_extension_max_len));
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
        if (job.query_len > k_gpu_alignment_simple_cigar_max_len ||
            job.target_len > k_gpu_alignment_simple_cigar_max_len) {
            throw vulkan_runtime_error(
                fmt::format("alignment CIGAR extension job {} length {}x{} exceeds {}",
                            i,
                            job.query_len,
                            job.target_len,
                            k_gpu_alignment_simple_cigar_max_len));
        }
        if (job.band_width > params.max_band_width) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR extension job {} band_width {} exceeds max_band_width {}",
                i,
                job.band_width,
                params.max_band_width));
        }
        if (job.cigar_capacity == 0) {
            throw vulkan_runtime_error(
                fmt::format("alignment CIGAR extension job {} CIGAR capacity is zero", i));
        }
        const bool bounded_dp_candidate = job.query_len <= k_gpu_alignment_extension_max_len &&
                                          job.target_len <= k_gpu_alignment_extension_max_len;
        const std::uint64_t traceback_cells =
            bounded_dp_candidate ? (static_cast<std::uint64_t>(job.query_len) + 1u) *
                                       (static_cast<std::uint64_t>(job.target_len) + 1u)
                                 : 0u;
        const std::uint64_t traceback_end = static_cast<std::uint64_t>(job.traceback_tile_offset) +
                                            static_cast<std::uint64_t>(job.traceback_tile_count);
        if (bounded_dp_candidate && job.traceback_tile_count < traceback_cells) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR extension job {} traceback capacity {} below required {}",
                i,
                job.traceback_tile_count,
                traceback_cells));
        }
        if (traceback_end > std::numeric_limits<std::uint32_t>::max()) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR extension job {} traceback range [{}, {}) exceeds uint32",
                i,
                job.traceback_tile_offset,
                traceback_end));
        }
        const std::uint64_t cigar_end = static_cast<std::uint64_t>(job.cigar_offset) +
                                        static_cast<std::uint64_t>(job.cigar_capacity);
        if (cigar_end > packed_cigar_capacity) {
            throw vulkan_runtime_error(fmt::format(
                "alignment CIGAR extension job {} CIGAR range [{}, {}) exceeds capacity {}",
                i,
                job.cigar_offset,
                cigar_end,
                packed_cigar_capacity));
        }
        validate_alignment_extension_symbols(query_symbols,
                                             job.query_symbol_offset,
                                             job.query_len,
                                             params.alphabet_size,
                                             "query",
                                             i);
        validate_alignment_extension_symbols(target_symbols,
                                             job.target_symbol_offset,
                                             job.target_len,
                                             params.alphabet_size,
                                             "target",
                                             i);
    }
}

/// @brief Device byte footprint for `count` packed minimizers of the given word kind.
/// @return Saturating device byte footprint.
[[nodiscard]] std::uint64_t minimizer_device_bytes(std::uint32_t word_kind,
                                                   std::uint32_t words32_per_minimizer,
                                                   std::uint64_t count) noexcept {
    if (word_kind == 0u) { // u16: two minimizers packed per 32-bit word
        return ((count + 1u) / 2u) * sizeof(std::uint32_t);
    }
    return count * static_cast<std::uint64_t>(words32_per_minimizer) * sizeof(std::uint32_t);
}

/// @brief Host byte footprint for `count` minimizers (one packed element each).
/// @return Saturating host byte footprint.
[[nodiscard]] std::uint64_t minimizer_host_bytes(std::uint32_t word_kind,
                                                 std::uint64_t count) noexcept {
    switch (word_kind) {
    case 0u:
        return count * 2u; // u16
    case 1u:
        return count * 4u; // u32
    case 2u:
        return count * 8u; // u64
    default:
        return count * 16u; // u64x2
    }
}

constexpr VkBufferUsageFlags k_gpu_arena_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

struct gpu_sketch_batch_arena_request {
    bool valid = false;
    std::uint32_t num_blocks = 0;
    std::uint64_t requested_bytes = 0;
};

/// @brief Authoritative eight-slice request shared by admission planning and the
/// dispatch itself: sequence, LUT, tile records, counts, offsets, scan blocks,
/// counters, and pair-value scratch.
/// @return Validated arena request or an invalid request on overflow.
[[nodiscard]] gpu_sketch_batch_arena_request
make_sketch_batch_arena_request(std::uint64_t packed_sequence_bytes,
                                std::uint64_t tile_count) noexcept {
    constexpr std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)();
    if (tile_count == 0u ||
        tile_count > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return {};
    }
    const std::uint32_t num_blocks =
        std::max<std::uint32_t>(bioseq::common::ceil_div(tile_count, k_prefix_scan_block_size), 1u);
    const auto append_slice = [](std::uint64_t total,
                                 std::uint64_t bytes) noexcept -> std::uint64_t {
        const std::uint64_t aligned_bytes =
            bioseq::common::checked_align_up(std::max<std::uint64_t>(bytes, 4u),
                                             k_gpu_buffer_offset_alignment)
                .value_or((std::numeric_limits<std::uint64_t>::max)());
        return bioseq::common::saturating_add(total, aligned_bytes);
    };
    const std::uint64_t tile_bytes =
        bioseq::common::saturating_mul(tile_count, sizeof(gpu_sketch_tile_wire));
    const std::uint64_t tile_u32_bytes =
        bioseq::common::saturating_mul(tile_count, sizeof(std::uint32_t));
    const std::uint64_t block_bytes =
        bioseq::common::saturating_mul(num_blocks, sizeof(std::uint32_t));
    std::uint64_t requested_bytes = 0u;
    requested_bytes = append_slice(requested_bytes, packed_sequence_bytes);
    requested_bytes = append_slice(requested_bytes, sizeof(vulkan_rank_lut));
    requested_bytes = append_slice(requested_bytes, tile_bytes);
    requested_bytes = append_slice(requested_bytes, tile_u32_bytes);
    requested_bytes = append_slice(requested_bytes, tile_u32_bytes);
    requested_bytes = append_slice(requested_bytes, block_bytes);
    requested_bytes = append_slice(requested_bytes, 8u);
    requested_bytes = append_slice(requested_bytes, 16u);
    if (tile_bytes == limit || tile_u32_bytes == limit || block_bytes == limit ||
        requested_bytes == limit) {
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

/// @brief Derives the exact radix width for encoded target coordinates.
/// @return Conservative default when the coordinate span is not representable.
[[nodiscard]] gpu_anchor_sort_key_plan
make_anchor_sort_key_plan(std::uint64_t target_len, std::uint64_t target_position_offset) noexcept {
    const auto pass_count =
        runtime_grouped_soa_radix_pass_count(target_position_offset, target_len);
    if (!pass_count.has_value()) {
        return gpu_anchor_sort_key_plan{};
    }

    const std::uint64_t max_target_coordinate = target_position_offset + target_len - 1u;
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
    VkMemoryBarrier memory_barrier{};
    memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memory_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    memory_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         1,
                         &memory_barrier,
                         0,
                         nullptr,
                         0,
                         nullptr);
}

/// @brief Records host-write visibility before a shader consumes mapped bytes.
void record_host_to_shader(VkCommandBuffer cmd) {
    VkMemoryBarrier memory_barrier{};
    memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memory_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         1,
                         &memory_barrier,
                         0,
                         nullptr,
                         0,
                         nullptr);
}

/// @brief Records shader-write visibility before the host reads mapped bytes.
void record_shader_to_host(VkCommandBuffer cmd) {
    VkMemoryBarrier memory_barrier{};
    memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memory_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memory_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         1,
                         &memory_barrier,
                         0,
                         nullptr,
                         0,
                         nullptr);
}

/// @brief Picks `(num_workgroups, elems_per_wg)` for a device sort or scan,
/// keeping the workgroup count under the device dispatch limit.
/// @param[in] element_count Number of elements covered by the dispatch.
/// @param[in] maximum_workgroup_count Device X-dimension workgroup limit.
/// @return Dispatch workgroup count and elements assigned per workgroup.
struct workgroup_plan {
    std::uint32_t num_workgroups;
    std::uint32_t elems_per_wg;
};
[[nodiscard]] workgroup_plan plan_workgroups(std::uint64_t element_count,
                                             std::uint32_t maximum_workgroup_count) {
    if (element_count == 0)
        return workgroup_plan{0u, 1u};
    // Small per-workgroup chunks maximise concurrency for the 1-lane histogram
    // and stable scatter; the parallel scan keeps the per-bin reduction cheap
    // even with many workgroups.
    std::uint32_t elements_per_workgroup = 512u;
    std::uint32_t workgroup_count =
        bioseq::common::ceil_div(element_count, elements_per_workgroup);
    if (workgroup_count > maximum_workgroup_count) {
        workgroup_count = maximum_workgroup_count;
        elements_per_workgroup = bioseq::common::ceil_div(element_count, workgroup_count);
    }
    return workgroup_plan{workgroup_count, elements_per_workgroup};
}

/// @brief Next power-of-two open-addressed slot capacity for `distinct` buckets,
/// matching `indexing::gpu_slot_capacity` (load factor < 0.5).
/// @return Slot capacity, or zero when the requirement is not representable.
[[nodiscard]] std::uint32_t sketch_slot_capacity(std::uint64_t distinct) {
    const std::uint64_t wanted =
        bioseq::common::saturating_add(bioseq::common::saturating_mul(distinct, 2u), 1u);
    std::uint64_t cap = 2;
    while (cap < wanted)
        cap <<= 1;
    return static_cast<std::uint32_t>(cap);
}

/// @brief Submit an executable command buffer and wait for the synchronous runtime
/// contract. The command buffer remains executable and may be submitted again.
void submit_executable_and_wait(VkDevice device,
                                VkQueue queue,
                                VkCommandBuffer cmd,
                                VkFence fence,
                                const char* what,
                                runtime_pipeline_use_collector* collector = nullptr) {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check_vk(vkQueueSubmit(queue, 1, &submit, fence), what);
    check_vk(vkWaitForFences(device,
                             1,
                             &fence,
                             VK_TRUE,
                             std::numeric_limits<std::uint64_t>::max()),
             what);
    check_vk(vkResetFences(device, 1, &fence), what);
    // Restore the shared fence before any policy check can throw so the next
    // synchronous operation never inherits a signalled fence.
    require_immutable_metal_policy_live(what);
    if (collector != nullptr)
        collector->commit(cmd);
}

/// @brief End the currently-recording command buffer, submit it, and wait. This keeps
/// the established helper contract for GPU frontend's multi-submit operations.
void submit_and_wait(VkDevice device,
                     VkQueue queue,
                     VkCommandBuffer cmd,
                     VkFence fence,
                     const char* what,
                     runtime_pipeline_use_collector* collector = nullptr) {
    check_vk(vkEndCommandBuffer(cmd), what);
    submit_executable_and_wait(device, queue, cmd, fence, what, collector);
}

} // namespace bioseq::gpu::vulkan

/**
 * @file support.cppm
 * @brief Loads Vulkan functions and provides shared input checks and GPU submission helpers.
 */
module;

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#if defined(__APPLE__) && !defined(BIOSEQ_RECEIPT_OWNED_VULKAN_LOADER_PATH)
#error "Apple Vulkan runtime requires one receipt-owned loader path"
#endif

module bioseq.gpu.vulkan.runtime:support;

import :frontend_types;
import :metal_archive;
import bioseq.gpu.vulkan.shader_cache;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.alignment.utils;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/** Reject incompatible native hash, physical packing and rank-policy contracts. */
inline void validate_native_sketch_contract(const vulkan_rank_lut& policy,
                                             const vulkan_sketch_params& params) {
    if (params.kmer_size == 0u || params.kmer_size > 31u || params.window_size == 0u ||
        params.window_size > 64u || params.hash_bits < 32u || params.hash_bits > 64u ||
        params.pos_bits == 0u || params.pos_bits > 63u ||
        (params.unit_bits != 8u && params.unit_bits != 16u && params.unit_bits != 32u && params.unit_bits != 64u) ||
        params.bits_per_char == 0u || params.bits_per_char > 8u ||
        params.letters_per_unit != params.unit_bits / params.bits_per_char ||
        params.char_mask != (std::uint32_t{1u} << params.bits_per_char) - 1u ||
        policy.alphabet_size == 0u || policy.alphabet_size > policy.rank_to_code.size() ||
        policy.alphabet_size > params.char_mask + 1u || params.alphabet_size != policy.alphabet_size ||
        policy.seed_policy_id == 0u || policy.canonical_reverse_complement > 1u ||
        (params.minimizer_word_kind != 2u && params.minimizer_word_kind != 3u) ||
        (params.minimizer_word_kind == 2u && (params.minimizer_words32 != 2u ||
            params.hash_shift != params.pos_bits + 1u || params.hash_bits + params.pos_bits + 1u > 64u)) ||
        (params.minimizer_word_kind == 3u && (params.minimizer_words32 != 4u || params.hash_shift != 0u)))
        throw vulkan_runtime_error("GPU sketch native rank/hash contract is invalid");
    for (std::size_t rank = 0u; rank < policy.alphabet_size; ++rank)
        if (policy.rank_to_code[rank] != rank || (policy.canonical_reverse_complement != 0u &&
            policy.rank_to_rc_code[rank] >= policy.alphabet_size))
            throw vulkan_runtime_error("GPU sketch native rank policy is invalid");
}

/** Check one borrowed packed logical source without expanding symbol ranks. */
inline void validate_native_sketch_window(std::span<const std::byte> packed,
    const vulkan_rank_lut& policy, const vulkan_sketch_params& params,
    std::uint32_t symbol_count, std::uint32_t word_offset) {
    const auto units = bioseq::common::ceil_div(std::uint64_t{symbol_count},
                                               std::uint64_t{params.letters_per_unit});
    const auto bytes = units * (params.unit_bits / 8u);
    const auto offset = std::uint64_t{word_offset} * sizeof(std::uint32_t);
    if (offset > packed.size() || bytes > packed.size() - offset)
        throw vulkan_runtime_error("GPU sketch packed source is truncated");
    const gpu_sequence_view_wire descriptor{0u, bytes, (bytes + 3u) / 4u * 4u, units,
        0u, symbol_count, params.letters_per_unit, params.bits_per_char, params.unit_bits,
        policy.alphabet_size, params.char_mask,
        static_cast<std::uint32_t>(units * params.letters_per_unit - symbol_count),
        params.alphabet_kind, 0u, policy.alphabet_code, policy.seed_policy_id};
    if (!packed_sequence_upload_is_valid({descriptor, packed.subspan(offset, bytes)}))
        throw vulkan_runtime_error("GPU sketch packed source contains invalid native ranks");
}

#ifdef BIOSEQ_TEST_INDEXES
/// @brief Counters and pause controls for tests that inject a submission  failure.
/// Atomic fields let the test thread observe and release the runtime thread
/// without taking the runtime mutex.
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

/// Shared state for submission-ailure tests.
vulkan_submission_test_state g_vulkan_submission_test_state{};

/// @brief Returns the submission failure currently selected by the test.
[[nodiscard]] vulkan_submission_test_fault active_vulkan_submission_test_fault() noexcept {
    return static_cast<vulkan_submission_test_fault>(
        g_vulkan_submission_test_state.fault.load(std::memory_order_acquire));
}

/// @brief Counts an upload while a test fault is armed and records an upload failure if requested.
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

/// @brief Pauses an armed test after queue submission until the controller releases it.
/// @return Failure to report after the fence has been waited for and reset.
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

/// @brief Pauses an armed test  after the fence wait until failure delivery is released.
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

/// @brief Throws the injected failure after the, caller has finished queue and fence cleanup.
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

/// @brief Turns a failed Vulkan result into an exception with the operation name.
/// @param[in] result Result returned by Vulkan.
/// @param[in] what Operation name to include in the error.
/// @throws vulkan_runtime_error If result is not VK_SUCCESS.
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

/// Instance extension that exposes portability devices such as MoltenVK.
constexpr const char* k_portability_enumeration = "VK_KHR_portability_enumeration";
/// Instance extension used to query chained physical-device properties.
constexpr const char* k_get_physical_device_properties2 = "VK_KHR_get_physical_device_properties2";
/// Device extension required by portability implementations.
constexpr const char* k_portability_subset = "VK_KHR_portability_subset";
/// Instance extension used to pass loader layer settings.
constexpr const char* k_layer_settings = VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;

/// @brief Checks whether an extension list contains the requested name.
/// @param[in] available Extension properties from Vulkan.
/// @param[in] name Null-terminated extension name.
[[nodiscard]] bool has_extension(const std::vector<VkExtensionProperties>& available,
                                 const char* name) noexcept {
    for (const auto& ext : available) {
        if (std::strcmp(ext.extensionName, name) == 0)
            return true;
    }
    return false;
}

/// @brief Lists available Vulkan instance extensions, returning an empty list on failure.
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

/// @brief Lists extensions for a physical device, returning an empty list on  failure.
/// @param[in] device Physical device to query.
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

/// @brief Helps the Vulkan loader find Homebrew's MoltenVK driver on macOS.
/// Sets VK_ICD_FILENAMES to an existing Homebrew manifest if the varikble is
/// absent Also disables Metal argument buffers unless the user already set
/// MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS.
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
/// @brief Resets counters and pauses, then arms the requested submission failure.
/// @param[in] fault Failure for the next matching test submission.
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

/// @brief Releases the test pause before the fence wait, recording cancellation if requested.
void release_vulkan_submission_test_fence() noexcept {
    auto& state = g_vulkan_submission_test_state;
    if (active_vulkan_submission_test_fault() ==
        vulkan_submission_test_fault::cancellation_after_submit) {
        state.cancellation_requested.store(1u, std::memory_order_release);
    }
    state.release_gate.store(1u, std::memory_order_release);
    state.release_gate.notify_all();
}

/// @brief Releases the test pause before reporting the injected failure.
void release_vulkan_submission_test_delivery() noexcept {
    auto& state = g_vulkan_submission_test_state;
    state.delivery_gate.store(1u, std::memory_order_release);
    state.delivery_gate.notify_all();
}

/// @brief Clears the injected failure and counters and releases both test pauses.
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

/// @brief Reads the current test  failure, pause flags, and counters.
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

/// @brief Loads required Vulkan entry points and reports any missing symbols.
void load_vulkan_runtime_functions(void* loader_handle) {
    if (loader_handle == nullptr) {
        throw vulkan_runtime_error("Vulkan loader handle is null");
    }
    std::string missing;
#define BIOSEQ_LOAD_VULKAN_FUNCTION(name)                                                          \
    do {                                                                                           \
        name = reinterpret_cast<PFN_##name>(::dlsym(loader_handle, #name));                        \
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

/// @brief Opens the Vulkan loader and resolves the functions used by the runtime.
/// On Apple, checks that vkCreateInstance belongs to the configured loader file.
void ensure_vulkan_loader_loaded_for_runtime() {
#if defined(__APPLE__)
    std::error_code error;
    const std::filesystem::path required_loader_path = selected_vulkan_loader_path(error);
    if (error || !required_loader_path.is_absolute()) {
        throw vulkan_runtime_error("receipt-owned Vulkan loader path is not canonical");
    }
    void* loader_handle = ::dlopen(required_loader_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    void* create_instance =
        loader_handle == nullptr ? nullptr : ::dlsym(loader_handle, "vkCreateInstance");
    Dl_info loader_info{};
    if (create_instance == nullptr || ::dladdr(create_instance, &loader_info) == 0 ||
        loader_info.dli_fname == nullptr) {
        throw vulkan_runtime_error(
            "receipt-owned Vulkan loader does not own vkCreateInstance");
    }
    error.clear();
    const std::filesystem::path observed_loader_path =
        std::filesystem::canonical(loader_info.dli_fname, error);
    if (error || observed_loader_path != required_loader_path) {
        throw vulkan_runtime_error(
            "resolved vkCreateInstance image differs from the receipt-owned Vulkan loader");
    }
#else
    void* loader_handle = RTLD_DEFAULT;
    if (::dlsym(loader_handle, "vkCreateInstance") == nullptr) {
        constexpr std::array candidates = {"libvulkan.so.1", "libvulkan.so"};
        std::string errors;
        for (const char* path : candidates) {
            ::dlerror();
            if (void* loaded = ::dlopen(path, RTLD_NOW | RTLD_GLOBAL); loaded != nullptr) {
                loader_handle = loaded;
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
        if (::dlsym(loader_handle, "vkCreateInstance") == nullptr) {
            throw vulkan_runtime_error(
                fmt::format("Vulkan loader unavailable for backend runtime initialization: {}",
                            errors.empty() ? "no dlopen diagnostics" : errors));
        }
    }
#endif
    load_vulkan_runtime_functions(loader_handle);
}

/// @brief Returns the library path containing a symbol on Apple.
/// @param[in] symbol Loaded symbol address.
/// @return Owning library path, or an empty string if unavailable or on another platform.
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

/// @brief Copies host bytes into a mapped Vulkan buffer.
/// @param[in,out] transfer_scratch Unused with the current mapped-memory path.
/// @param[out] dst Mapped destination buffer.
/// @param[in] bytes Source bytes, which must fit in dst.
/// @param[in] dst_access Unused with the current mapped-memory zath.
/// @throws vulkan_runtime_error If a nonempty copy has no destination mapping.
void runtime::upload_locked(buffer& transfer_scratch,
                            buffer& dst,
                            std::span<const std::byte> bytes,
                            VkAccessFlags dst_access) {
    (void)transfer_scratch; // The copy writes mapped memory directly.
    (void)dst_access;       // Host-coherent writes become  visible when the queue is submitted.
    if (bytes.empty())
        return;
    if (dst.mapped == nullptr) {
        // This upload path requires a host mapping. Callers can pass an empty
        // scratch buffer because, no staging transfer is used.
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

/// @brief Copies a mapped Vulkan buffer into caller-owned host memory
/// The caller must wait for GPU writes to finish first.
/// @param[in,out] transfer_scratch Unused with the current mapped-memory path.
/// @param[in] src Mapped source buffer.
/// @param[out] bytes Destination span, which must fit within src.
/// @throws vulkan_runtime_error If a nonempty copy has no source mapping.
void runtime::download_locked(buffer& transfer_scratch,
                              const buffer& src,
                              std::span<std::byte> bytes) {
    (void)transfer_scratch; // The copy reads mapped memory directly.
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

// Shared shader parameters and input checks.

/// @brief Parameters passed to the seed-lookup  shader.
/// Field order and widths must match the shader's push-constant block.
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
    std::uint32_t query_minimizer_record_offset;
};
static_assert(sizeof(seed_lookup_push_constants) == 48);

/// Buffer uses needed for shader storage and copies in either direction.
constexpr VkBufferUsageFlags k_compute_buffer_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;

/// @brief Parameters passed to the score-only alignment shader.
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

/// @brief Parameters passed to the alignment shader that writes CIGAR operations.
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
    std::array<std::uint32_t, 8> complement_rank_words{};
};
static_assert(sizeof(alignment_cigar_push_constants) == 92);
static_assert(std::is_standard_layout_v<alignment_cigar_push_constants>);

/// @brief Copies alignment settings into the shader's push-constant layout.
/// @param[in] job_count Number of jobs in the dispatch.
/// @param[in] params Validated sequence limits and scoring settings.
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

/// @brief Copies CIGAR alignment settings into the shader's push-constant laytut.
/// @param[in] job_count Number of jobs in the dispatch.
/// @param[in] params Validated scoring and tile settings.
[[nodiscard]] alignment_cigar_push_constants
alignment_cigar_push(std::uint32_t job_count, const gpu_alignment_cigar_params& params) noexcept {
    auto canonical = params;
    if (canonical.gap_open_short <= canonical.gap_open_long && canonical.gap_extend_short <= canonical.gap_extend_long) {
        canonical.gap_open_long = canonical.gap_open_short;
        canonical.gap_extend_long = canonical.gap_extend_short;
    } else if (canonical.gap_open_long <= canonical.gap_open_short && canonical.gap_extend_long <= canonical.gap_extend_short) {
        canonical.gap_open_short = canonical.gap_open_long;
        canonical.gap_extend_short = canonical.gap_extend_long;
    }
    return alignment_cigar_push_constants{job_count,
                                          params.alphabet_size,
                                          params.max_band_width,
                                          params.tile_query,
                                          params.tile_target,
                                          params.match_score,
                                          params.mismatch_penalty,
                                          canonical.gap_open_short,
                                          canonical.gap_extend_short,
                                          canonical.gap_open_long,
                                          canonical.gap_extend_long,
                                          params.z_drop,
                                          params.end_bonus,
                                          params.flags,
                                          params.reserved,
                                          params.complement_rank_words};
}

/** Check wire layout, registered identity and signed scores against prepared metadata. */
[[nodiscard]] std::expected<bioseq::simd::substitution_summary, bioseq::simd::alignment_contract_error> validate_gpu_substitution_matrix(
    const gpu_substitution_matrix& matrix, std::uint32_t alphabet_size,
    std::int32_t match_score, std::int32_t mismatch_penalty) {
    const auto& header = matrix.header();
    if (header.model > 1 || header.reserved != 0 || header.rank_count != alphabet_size
        || header.alphabet_code >= supported_biological_alphabets::alphabet_count)
        return std::unexpected(bioseq::simd::alignment_contract_error::invalid_matrix_layout);
    bool identity_matches = false;
    supported_biological_alphabets::dispatch_alphabet_by_code(
        static_cast<supported_biological_alphabets::alphabet_code_t>(header.alphabet_code),
        [&identity_matches, alphabet_size]<class Alphabet>() {
            identity_matches = seqan3::alphabet_size<Alphabet> == alphabet_size;
        });
    if (!identity_matches) return std::unexpected(bioseq::simd::alignment_contract_error::invalid_matrix_layout);
    const auto& summary = matrix.summary();
    if (summary.minimum < -127 || summary.maximum > 127 || summary.minimum > summary.maximum)
        return std::unexpected(bioseq::simd::alignment_contract_error::invalid_matrix_layout);
    if (header.model == 0) {
        if (header.row_stride != 0 || header.element_count != 0 || matrix.values().size() != 1
            || matrix.values().front() != 0 || !summary.uniform_diagonal || !summary.uniform_off_diagonal
            || !summary.uniform_unknown || match_score != summary.equal_score
            || std::int64_t{mismatch_penalty} != -std::int64_t{summary.unequal_score})
            return std::unexpected(bioseq::simd::alignment_contract_error::invalid_matrix_layout);
    } else {
        const std::uint64_t count = std::uint64_t{header.rank_count} * header.rank_count;
        if (header.row_stride != header.rank_count || header.element_count != count || matrix.values().size() != count)
            return std::unexpected(bioseq::simd::alignment_contract_error::invalid_matrix_layout);
        for (const auto value : matrix.values())
            if (value < summary.minimum || value > summary.maximum || value < -127 || value > 127)
                return std::unexpected(bioseq::simd::alignment_contract_error::invalid_matrix_layout);
    }
    return summary;
}

/** Check the shared int32 accumulation envelope before any GPU allocation. */
[[nodiscard]] std::expected<void, bioseq::simd::alignment_contract_error> validate_gpu_alignment_numeric(
    const bioseq::simd::substitution_summary& summary,
    std::size_t query_length, std::size_t target_length,
    std::int32_t gap_open, std::int32_t gap_extend, std::int32_t end_bonus = 0,
    std::int32_t z_drop = -1, std::int32_t gap_open_long = -1, std::int32_t gap_extend_long = -1) {
    return bioseq::simd::validate_alignment_numeric_contract(summary, query_length, target_length,
        gap_open, gap_extend, end_bonus, z_drop, gap_open_long, gap_extend_long, false);
}

/** Borrow one job's native packed window for the shared descriptor upload. */
[[nodiscard]] std::expected<gpu_packed_sequence_upload, bioseq::simd::alignment_contract_error> alignment_packed_upload(
    std::span<const std::byte> payload, const gpu_alignment_cigar_job_wire& job,
    bool query, const gpu_substitution_matrix& matrix) {
    const std::size_t offset = query ? job.query_symbol_offset : job.target_symbol_offset;
    const std::size_t bytes = query ? job.query_packed_byte_count : job.target_packed_byte_count;
    const std::uint32_t unit_bytes = query ? job.query_storage_unit_width : job.target_storage_unit_width;
    const std::uint32_t leading = query ? job.query_leading_symbol_offset : job.target_leading_symbol_offset;
    const std::uint32_t length = query ? job.query_len : job.target_len;
    if (offset > payload.size() || bytes > payload.size() - offset || (unit_bytes != 1 && unit_bytes != 2 && unit_bytes != 4 && unit_bytes != 8)
        || job.bits_per_symbol == 0 || job.bits_per_symbol > 8)
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    const std::uint32_t letters = unit_bytes * 8u / job.bits_per_symbol;
    if (letters == 0 || bytes % unit_bytes != 0 || bytes / unit_bytes * letters < std::uint64_t{leading} + length)
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    gpu_packed_sequence_upload upload{};
    upload.bytes = payload.subspan(offset, bytes);
    upload.sequence = {
        .byte_count = bytes, .padded_byte_count = (bytes + 3u) / 4u * 4u,
        .unit_count = bytes / unit_bytes,
        .bp_offset = query ? job.query_global_start : job.target_global_start,
        .bp_count = length, .letters_per_unit = letters, .bits_per_char = job.bits_per_symbol,
        .unit_bits = unit_bytes * 8u, .alphabet_size = matrix.header().rank_count,
        .char_mask = (std::uint32_t{1} << job.bits_per_symbol) - 1u,
        .padding_letters = static_cast<std::uint32_t>(bytes / unit_bytes * letters - leading - length),
        .alphabet_kind = static_cast<std::uint32_t>(alphabet_kernel_kind::generic),
        .leading_symbol_offset = leading, .alphabet_code = matrix.header().alphabet_code,
    };
    if (!packed_sequence_upload_is_valid(upload)) return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    return upload;
}

/// @brief Checks score-only alignment jobs and their sequence slices before GPU submission.
[[nodiscard]] std::expected<void, bioseq::simd::alignment_contract_error> validate_alignment_extension_inputs(const gpu_packed_sequence_upload& query_symbols,
                                         const gpu_packed_sequence_upload& target_symbols,
                                         std::span<const gpu_alignment_extension_job_wire> jobs,
                                         const gpu_alignment_extension_params& params,
                                         const gpu_substitution_matrix& matrix) {
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }
    if (query_symbols.sequence.bp_count > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }
    if (target_symbols.sequence.bp_count > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }
    if (params.max_query_len == 0 || params.max_query_len > k_gpu_alignment_extension_max_len) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }
    if (params.max_target_len == 0 || params.max_target_len > k_gpu_alignment_extension_max_len) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }
    if (params.max_band_width > k_gpu_alignment_extension_max_band_width) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }
    if (params.alphabet_size == 0 || params.alphabet_size > 256u) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }

    if (params.flags != 0 || params.reserved0 != 0) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }

    const auto summary = validate_gpu_substitution_matrix(matrix, params.alphabet_size, params.match_score, params.mismatch_penalty);
    if (!summary) return std::unexpected(summary.error());
    if (!packed_sequence_upload_is_valid(query_symbols) || !packed_sequence_upload_is_valid(target_symbols)
        || query_symbols.sequence.alphabet_code != matrix.header().alphabet_code
        || target_symbols.sequence.alphabet_code != matrix.header().alphabet_code)
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const gpu_alignment_extension_job_wire& job = jobs[i];
        if (job.query_len > params.max_query_len) {
            return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
        }
        if (job.target_len > params.max_target_len) {
            return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
        }
        if (job.band_width > params.max_band_width) {
            return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
        }
        if (job.flags != 0 || job.reserved0 != 0 || job.reserved1 != 0) {
            return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
        }
        if (job.query_offset > query_symbols.sequence.bp_count
            || job.query_len > query_symbols.sequence.bp_count - job.query_offset
            || job.target_offset > target_symbols.sequence.bp_count
            || job.target_len > target_symbols.sequence.bp_count - job.target_offset)
            return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
        const auto numeric = validate_gpu_alignment_numeric(*summary, job.query_len, job.target_len,
            params.gap_open_penalty, params.gap_extend_penalty);
        if (!numeric) return std::unexpected(numeric.error());
    }
    return {};
}

/** Validates native packed geometry, ranks, record identity and job coordinates before dispatch. */
void validate_prefilter_inputs(const char* algorithm,
                               std::span<const gpu_packed_sequence_upload> queries,
                               std::span<const gpu_packed_sequence_upload> targets,
                               const vulkan_rank_lut& rank_lut,
                               std::span<const bioseq::prefilter::prefilter_job> jobs) {
    constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
    if (jobs.size() > limit || queries.size() > limit || targets.size() > limit ||
        rank_lut.alphabet_size == 0u || rank_lut.alphabet_size > vulkan_rank_lut::rank_capacity) {
        throw vulkan_runtime_error(fmt::format("{} invalid record counts or alphabet", algorithm));
    }
    const auto validate_records = [algorithm, &rank_lut](auto records) {
        for (const auto& upload : records) {
            const auto& sequence = upload.sequence;
            if (!packed_sequence_upload_is_valid(upload) ||
                sequence.alphabet_code != rank_lut.alphabet_code ||
                sequence.alphabet_size != rank_lut.alphabet_size ||
                sequence.seed_policy_id != rank_lut.seed_policy_id) {
                throw vulkan_runtime_error(fmt::format("{} invalid native packed sequence", algorithm));
            }
        }
    };
    validate_records(queries);
    validate_records(targets);
    for (const auto& job : jobs) {
        if (job.query_record_index >= queries.size() || job.target_record_index >= targets.size() ||
            !bioseq::prefilter::prefilter_slice_in_bounds(queries[job.query_record_index].sequence.bp_count,
                                                         job.query_start, job.query_length) ||
            !bioseq::prefilter::prefilter_slice_in_bounds(targets[job.target_record_index].sequence.bp_count,
                                                         job.target_start, job.target_length) ||
            (job.orientation != 0u && (job.orientation != 3u || rank_lut.canonical_reverse_complement == 0u))) {
            throw vulkan_runtime_error(fmt::format("{} invalid packed job coordinates or orientation", algorithm));
        }
    }
}

/// @brief Checks SneakySnake results against each job's supported shape and edit limit.
/// Unsupported jobs must remain accepted so the filter cannot discard them.
void validate_sneaky_snake_results(std::span<const bioseq::prefilter::prefilter_job> jobs,
                                   std::span<const bioseq::prefilter::prefilter_result> results) {
    using bioseq::prefilter::prefilter_status;
    if (results.size() != jobs.size()) {
        throw vulkan_runtime_error("SneakySnake result count mismatch");
    }
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const bioseq::prefilter::prefilter_job& job = jobs[i];
        const bioseq::prefilter::prefilter_result& result = results[i];
        const bool in_bounds = true; // Input geometry is checked before dispatch.
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

/// @brief Checks GateKeeper result flags and the reason for each rejection
/// Rejection needs a length difference beyond the edit limit, a  confirmed
/// edit-distance check, or the zero-edit Hamming check. Unsupported jobs
/// must remain accepted.
void validate_gatekeeper_results(std::span<const bioseq::prefilter::prefilter_job> jobs,
                                 std::span<const bioseq::prefilter::prefilter_result> results) {
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
        const bool in_bounds = true; // Input geometry is checked before dispatch.
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

/// @brief Checks packed sequence windows and alignment output sizes before submission.
/// Uses packing metadata to check byte ranges without unpacking the sequences.
[[nodiscard]] std::expected<void, bioseq::simd::alignment_contract_error> validate_alignment_packed_cigar_inputs(std::span<const std::byte> query_packed,
                                            std::span<const std::byte> target_packed,
                                            std::span<const gpu_alignment_cigar_job_wire> jobs,
                                            const gpu_alignment_cigar_params& params,
                                            const gpu_substitution_matrix& matrix,
                                            std::uint32_t packed_cigar_capacity) {
    if (jobs.empty() || jobs.size() > std::numeric_limits<std::uint32_t>::max() ||
        query_packed.empty() || target_packed.empty() ||
        query_packed.size() > std::numeric_limits<std::uint32_t>::max() ||
        target_packed.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }
    if (params.alphabet_size == 0u || params.alphabet_size > std::numeric_limits<std::uint8_t>::max() ||
        params.max_band_width > k_gpu_alignment_extension_max_band_width ||
        params.tile_query == 0u || params.tile_query > k_gpu_alignment_extension_max_len ||
        params.tile_target == 0u || params.tile_target > k_gpu_alignment_extension_max_len ||
        params.flags != 0u || params.reserved != 0u ||
        packed_cigar_capacity == 0u) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }

    const auto summary = validate_gpu_substitution_matrix(matrix, params.alphabet_size, params.match_score, params.mismatch_penalty);
    if (!summary) return std::unexpected(summary.error());
    const bool reverse_requested = std::ranges::any_of(jobs, [](const auto& job) {
        return (job.flags & k_gpu_alignment_cigar_job_flag_reverse_target) != 0;
    });
    if (reverse_requested) {
        bool complementable = false;
        supported_biological_alphabets::dispatch_alphabet_by_code(
            static_cast<supported_biological_alphabets::alphabet_code_t>(matrix.header().alphabet_code),
            [&complementable]<class Alphabet>() { complementable = seqan3::nucleotide_alphabet<Alphabet>; });
        if (!complementable || params.alphabet_size > params.complement_rank_words.size() * sizeof(std::uint32_t))
            return std::unexpected(bioseq::simd::alignment_contract_error::orientation);
    }
    for (std::uint32_t rank = 0u; reverse_requested && rank < params.alphabet_size; ++rank) {
        const std::uint32_t word = params.complement_rank_words[rank / 4u];
        const std::uint32_t complement = (word >> ((rank % 4u) * 8u)) & 0xffu;
        if (complement >= params.alphabet_size) {
            return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
        }
    }

    std::uint64_t prior_cigar_end = 0u;
    for (std::size_t i = 0u; i < jobs.size(); ++i) {
        const gpu_alignment_cigar_job_wire& job = jobs[i];
        const auto numeric = validate_gpu_alignment_numeric(*summary, job.query_len, job.target_len,
            params.gap_open_short, params.gap_extend_short, params.end_bonus, params.z_drop,
            params.gap_open_long, params.gap_extend_long);
        if (!numeric) return std::unexpected(numeric.error());
        const auto query_upload = alignment_packed_upload(query_packed, job, true, matrix);
        if (!query_upload) return std::unexpected(query_upload.error());
        const auto target_upload = alignment_packed_upload(target_packed, job, false, matrix);
        if (!target_upload) return std::unexpected(target_upload.error());
        const std::uint32_t length_delta = job.query_len > job.target_len
                                               ? job.query_len - job.target_len
                                               : job.target_len - job.query_len;
        if ((job.flags & k_gpu_alignment_cigar_job_flag_packed_inputs) == 0u ||
            (job.flags & ~k_gpu_alignment_cigar_job_flags) != 0u || job.query_len == 0u ||
            job.target_len == 0u || job.query_len > k_gpu_alignment_cigar_operation_max_len ||
            job.target_len > k_gpu_alignment_cigar_operation_max_len ||
            length_delta > job.band_width || job.band_width > params.max_band_width ||
            job.cigar_capacity == 0u) {
            return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
        }
        const std::uint64_t required_traceback =
            job.query_len <= k_gpu_alignment_extension_max_len &&
                    job.target_len <= k_gpu_alignment_extension_max_len
                ? (static_cast<std::uint64_t>(job.query_len) + 1u) *
                      (static_cast<std::uint64_t>(job.target_len) + 1u)
                : gpu_alignment_cigar_gridded_traceback_words(job.query_len,
                                                              job.target_len,
                                                              params.tile_query,
                                                              params.tile_target,
                                                              job.band_width);
        const std::uint64_t traceback_end =
            static_cast<std::uint64_t>(job.traceback_tile_offset) + job.traceback_tile_count;
        const std::uint64_t cigar_end =
            static_cast<std::uint64_t>(job.cigar_offset) + job.cigar_capacity;
        if (required_traceback == 0u ||
            required_traceback > (std::numeric_limits<std::uint32_t>::max)() ||
            job.traceback_tile_offset != 0u || job.traceback_tile_count != required_traceback ||
            traceback_end > std::numeric_limits<std::uint32_t>::max() ||
            job.cigar_offset != prior_cigar_end || cigar_end > packed_cigar_capacity ||
            job.query_global_start > (std::numeric_limits<std::uint32_t>::max)() - job.query_len ||
            job.target_global_start >
                (std::numeric_limits<std::uint32_t>::max)() - job.target_len) {
            return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
        }
        prior_cigar_end = cigar_end;
    }
    if (prior_cigar_end != packed_cigar_capacity) {
        return std::unexpected(bioseq::simd::alignment_contract_error::coordinates);
    }
    return {};
}

/// @brief Returns the device buffer size for a eiven minimizer count and encoding.
/// A final 16-bit minimizer still occupies a complete 32-bit shader  word.
/// @param[in] word_kind Minimizer encoding.
/// @param[in] words32_per_minimizer Words per record for encodings wider than 16 bits.
/// @param[in] count Number of minimizers.
/// @pre The byte calculation must fit in uint64_t, including count + 1 for 16-bit records.
[[nodiscard]] std::uint64_t minimizer_device_bytes(std::uint32_t word_kind,
                                                   std::uint32_t words32_per_minimizer,
                                                   std::uint64_t count) noexcept {
    if (word_kind == 0u) { // u16: two minimizers packed per 32-bit word
        return ((count + 1u) / 2u) * sizeof(std::uint32_t);
    }
    return count * static_cast<std::uint64_t>(words32_per_minimizer) * sizeof(std::uint32_t);
}

/// @brief Returns the host byte size of packed minimizers without device padding.
/// @param[in] word_kind Minimizer encoding.
/// @param[in] count Number of minimizers.
/// @pre The byte count must fit in uint64_t.
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

/// Buffer uses needed by arena slices for shader work, transfers, and indirect commands.
constexpr VkBufferUsageFlags k_gpu_arena_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

/// @brief Arena bytes and scan-block count required for a sketch batch.
struct gpu_sketch_batch_arena_request {
    bool valid = false;
    std::uint32_t num_blocks = 0;
    std::uint64_t requested_bytes = 0;
};

/// @brief Sizes the shared arena used to count and emit a sketch batch.
/// Includes the packed sequence, rank table, tile records and their counts,
/// plus scan and output bookkeeping. Planning and dispatch use this same layout.
/// @return The  arena request, or an invalid result for unsupported counts or overflow.
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
    // Include alignment padding for each slice and keep overflow detectable
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

/// Host type written into the sketch shader's tile-record buffer.
using sketch_tile_type = gpu_sketch_tile_wire;
static_assert(sizeof(sketch_tile_type) == 24);

/// @brief Parameters shared by the sketch counting and output shaders.
struct sketch_push_constants {
    std::uint32_t emit_mode, window_size, kmer_size, hash_bits, hash_shift, pos_bits;
    std::uint32_t word_kind, words32_per_minimizer, span_value, alphabet_kind;
    std::uint32_t unit_bits, bits_per_char, letters_per_unit, char_mask;
    std::uint32_t tile_count, emit_pairs, query_gate_mask, query_gate_word_count;
};
static_assert(sizeof(sketch_push_constants) == 72);

/// @brief Parameters for the block scan and its offset-addition stages.
struct scan_push_constants {
    std::uint32_t element_count, block_size, num_blocks, pass;
};
static_assert(sizeof(scan_push_constants) == 16);

/// @brief Parameters for counting and writing seed hits against a device index.
struct seed_push_constants {
    std::uint32_t words32_per_minimizer, word_kind, hash_shift, pos_bits, kmer_size;
    std::uint32_t slot_mask, max_occ, emit_mode, record_count, bucket_count;
    std::uint32_t target_pos_offset_lo, target_pos_offset_hi, target_ref_bits;
};
static_assert(sizeof(seed_push_constants) == 52);

/// @brief Bits and radix passes needed to sort encoded target positions.
struct gpu_anchor_sort_key_plan {
    std::uint32_t target_ref_bits = 63u;
    std::uint32_t pass_count = 8u;
};

/// @brief Chooses enough radix passes to sort positions kithin a target range.
/// @param[in] target_len Number of target positions.
/// @param[in] target_position_offset Coordinate of the first  target position.
/// @return A size-specific plan, or the full-width default if the range is unsupported.
/// @pre target_len is nonzero.
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

/// @brief Parameters shared by radix histogram, scan, and scatter stages.
struct sort_push_constants {
    std::uint32_t element_count, radix_shift, num_workgroups, elems_per_wg, value_words;
};
static_assert(sizeof(sort_push_constants) == 20);

/// @brief Parameters for grouping sorted minimizers and writing index buckets.
struct index_push_constants {
    std::uint32_t element_count, slot_mask, slot_capacity, num_buckets, probe_cap, pass;
};
static_assert(sizeof(index_push_constants) == 24);

/// @brief Host layout copied into the seed shader's per-query record buffer.
struct host_query_record {
    std::uint32_t min_offset, min_count, query_len, flags;
};
static_assert(sizeof(host_query_record) == 16);

/// @brief Makes writes from one compute stage visible to later compute stages.
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

/// @brief Makes host writes to mapped memory visible to following shader work.
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

/// @brief Makes shader writes visible to host reads after the GPU work completes.
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

/// @brief Number of workgroups and elements handled by each group.
struct workgroup_plan {
    std::uint32_t num_workgroups;
    std::uint32_t elems_per_wg;
};
/// @brief Splits sort or scan input across workgroups within the device, limit.
/// @param[in] element_count Number  of elements to process.
/// @param[in] maximum_workgroup_count Device limit along the X dimension.
/// @return Workgroup count and elements assigned to each group.
/// @pre The workgroup limit is nonzero for nonempty input.
[[nodiscard]] workgroup_plan plan_workgroups(std::uint64_t element_count,
                                             std::uint32_t maximum_workgroup_count) {
    if (element_count == 0)
        return workgroup_plan{0u, 1u};
    // Give each group a small chunk so histogram and scatter work can run
    // across many groups. Increase the chunk size if the device limit requires it
    std::uint32_t elements_per_workgroup = 512u;
    std::uint32_t workgroup_count =
        bioseq::common::ceil_div(element_count, elements_per_workgroup);
    if (workgroup_count > maximum_workgroup_count) {
        workgroup_count = maximum_workgroup_count;
        elements_per_workgroup = bioseq::common::ceil_div(element_count, workgroup_count);
    }
    return workgroup_plan{workgroup_count, elements_per_workgroup};
}

/// @brief Chooses a power-of-two slot table with room to keep it less than half qull.
/// @param[in] distinct Number of distinct target minimizer hashes.
/// @pre The resulting power-of-two capacity must fit in uint32_t.
[[nodiscard]] std::uint32_t sketch_slot_capacity(std::uint64_t distinct) {
    const std::uint64_t wanted =
        bioseq::common::saturating_add(bioseq::common::saturating_mul(distinct, 2u), 1u);
    std::uint64_t cap = 2;
    while (cap < wanted)
        cap <<= 1;
    return static_cast<std::uint32_t>(cap);
}

/// @brief Submits recorded GPU commands, waits for completion, and resets the fence.
/// The command buffer remains executable for reuse.
/// @param[in] device Device owning the fence and  command buffer.
/// @param[in] queue Queue compatible with the commands.
/// @param[in] cmd Executable command buffer.
/// @param[in] fence Unsignalled fence, reset after a successful wait.
/// @param[in] what Operation name used in errors and Metal checks.
/// @param[in,out] collector Optional recorder updated after completion and Metal checks.
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
    // Reset the shared fence before checking Metal settings, so a failed
    // settings check leaves the fence ready for the next submission.
    require_immutable_metal_policy_live(what);
    if (collector != nullptr)
        collector->commit(cmd);
}

/// @brief Finishes recording GPU commands, submits them, and waits for completion.
/// @param[in] device Device owning the fence and command buffer.
/// @param[in] queue Queue compatible with  the commands.
/// @param[in] cmd Command buffer currently being recorded.
/// @param[in] fence Unsignalled fence, reset after a successful wait.
/// @param[in] what Operation name used in errors and Metal checks.
/// @param[in,out] collector Optional recorder updated after completion and Metal checks.
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

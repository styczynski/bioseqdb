/**
 * @file device.cppm
 * @brief Owns Vulkan loader initialization, device lifetime, and buffer allocation.
 */
module;

#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:device;

import :frontend_types;
import :support;
import :metal_archive;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Releases one buffer through its owning runtime and clears every handle.
///
/// The mapped pointer and Vulkan handles become invalid before this function returns.
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

/// @brief Initializes the process runtime and retains any diagnostic on failure.
///
/// Construction is non-throwing by policy. An initialization failure leaves
/// `features().available` false and is exposed through the runtime diagnostic.
runtime::runtime() {
    try {
        init();
    } catch (const std::exception& exception) {
        init_error_ = exception.what();
        bioseq_log<logging_level::debug>("[vulkan_runtime] init failed: {}", exception.what());
        shutdown();
        features_ = vulkan_feature_set{};
    } catch (...) {
        init_error_ = "unknown Vulkan initialization failure";
        bioseq_log<logging_level::debug>("[vulkan_runtime] init failed (unknown)");
        shutdown();
        features_ = vulkan_feature_set{};
    }
}

/// @brief Releases runtime-owned Vulkan state in dependency-safe order.
runtime::~runtime() {
    shutdown();
}

/// @brief Returns the lazily initialized process-wide runtime owner.
/// @return Singleton whose lifetime extends through process teardown.
runtime& runtime::instance() {
    static runtime runtime;
    return runtime;
}

/// @brief Resolves the optional MoltenVK private workgroup-size entry point.
void runtime::resolve_moltenvk_private_api() noexcept {
    set_workgroup_size_mvk_ = nullptr;
#if defined(__APPLE__)
    // Resolves the private entry point from one already-open image handle.
    auto resolve_from = [](void* handle) noexcept -> set_workgroup_size_mvk_function {
        if (handle == nullptr)
            return nullptr;
        return reinterpret_cast<set_workgroup_size_mvk_function>(
            ::dlsym(handle, "vkSetWorkgroupSizeMVK"));
    };

    if (auto* fn = resolve_from(RTLD_DEFAULT); fn != nullptr) {
        set_workgroup_size_mvk_ = fn;
        return;
    }
    constexpr std::array candidates = {
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

/// @brief Checks whether the installed provider must supply shader modules.
/// @return `true` when a provider with a module loader is installed.
bool runtime::should_use_shader_provider_locked() const noexcept {
    runtime_shader_provider* provider = runtime_shader_provider_slot();
    return provider != nullptr && provider->load_module != nullptr;
}

/// @brief Lets the installed provider prepare shader state before runtime creation.
/// @throws vulkan_runtime_error When provider preparation reports failure.
void prepare_shader_provider_runtime() {
    runtime_shader_provider* provider = runtime_shader_provider_slot();
    if (provider == nullptr || provider->prepare_runtime == nullptr) {
        return;
    }
    auto prepared = provider->prepare_runtime();
    if (!prepared) {
        throw vulkan_runtime_error(
            fmt::format("runtime shader provider prepare failed: {}", prepared.error()));
    }
}

/// @brief Selects a physical device and creates the synchronous Vulkan runtime.
void runtime::init() {
    // --- Instance ---
    ensure_homebrew_moltenvk_icd_visible();
    ensure_vulkan_loader_loaded_for_runtime();
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
        if (!command_state.installed || !command_state.immutable_fail_closed) {
            throw vulkan_runtime_error("immutable GPU mode could not install fail-closed MoltenVK "
                                       "command-source interpose");
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
        if (command_state.sticky_failure.load(std::memory_order_acquire) ||
            command_state.source_fallback_calls.load(std::memory_order_relaxed) != 0u) {
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
            continue; // Vulkan chaining requires native 64-bit ints in the shader
        }

        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        if (family_count == 0)
            continue;
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        std::optional<std::uint32_t> compute_family;
        for (std::uint32_t family_index = 0; family_index < family_count; ++family_index) {
            if (families[family_index].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                compute_family = family_index;
                break;
            }
        }
        if (!compute_family)
            continue;

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
    selected_id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 selected_properties2{};
    selected_properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    selected_properties2.pNext = &selected_id_properties;
    vkGetPhysicalDeviceProperties2(physical_, &selected_properties2);
    runtime_vulkan_device_identity selected_device_identity{
        .api_version = selected_properties2.properties.apiVersion,
        .driver_version = selected_properties2.properties.driverVersion,
        .vendor_id = selected_properties2.properties.vendorID,
        .device_id = selected_properties2.properties.deviceID,
    };
    std::copy(std::begin(selected_id_properties.deviceUUID),
              std::end(selected_id_properties.deviceUUID),
              selected_device_identity.device_uuid.begin());
    std::copy(std::begin(selected_properties2.properties.pipelineCacheUUID),
              std::end(selected_properties2.properties.pipelineCacheUUID),
              selected_device_identity.pipeline_cache_uuid.begin());
    features_.device_uuid = selected_device_identity.device_uuid;
    features_.api_version = selected_device_identity.api_version;
    features_.driver_version = selected_device_identity.driver_version;
    features_.vendor_id = selected_device_identity.vendor_id;
    features_.device_id = selected_device_identity.device_id;
    features_.pipeline_cache_uuid = selected_device_identity.pipeline_cache_uuid;
#if defined(__APPLE__)
    if (is_runtime_metal_archive_configuration_installed() &&
        !bind_moltenvk_command_library_provenance(
            selected_device_identity,
            reinterpret_cast<const void*>(vkCreateInstance),
            reinterpret_cast<const void*>(set_workgroup_size_mvk_))) {
        throw vulkan_runtime_error(
            "immutable MoltenVK command-library provenance did not match the selected device");
    }
#endif
    install_metal_pipeline_archive_interpose(selected_device_identity);
    const VkResult create_device_result =
        vkCreateDevice(physical_, &device_info, nullptr, &device_);
#if defined(__APPLE__)
    if (create_device_result != VK_SUCCESS && is_runtime_metal_archive_configuration_installed()) {
        auto& archive_state = metal_pipeline_archive_interpose_state();
        mark_metal_archive_sticky_failure(
            archive_state,
            fmt::format("vkCreateDevice failed after immutable Metal archive install ({})",
                        static_cast<std::int32_t>(create_device_result)));
        rollback_metal_pipeline_archive_interpose_install(archive_state);
    }
    if (is_runtime_metal_archive_configuration_installed()) {
        auto& archive_state = metal_pipeline_archive_interpose_state();
        if (archive_state.sticky_failure.load(std::memory_order_acquire) ||
            archive_state.outside_scope_interceptions.load(std::memory_order_relaxed) != 0u) {
            if (!archive_state.sticky_failure.load(std::memory_order_acquire)) {
                mark_metal_archive_sticky_failure_literal(
                    archive_state,
                    "Metal pipeline interception escaped its scope during vkCreateDevice");
            }
            const std::string failure = metal_archive_failure_reason(archive_state);
            if (create_device_result == VK_SUCCESS && device_ != VK_NULL_HANDLE) {
                vkDestroyDevice(device_, nullptr);
                device_ = VK_NULL_HANDLE;
            }
            rollback_metal_pipeline_archive_interpose_install(archive_state);
            throw vulkan_runtime_error(fmt::format(
                "immutable Metal archive policy failed during vkCreateDevice: {}", failure));
        }
    }
#endif
    check_vk(create_device_result, "vkCreateDevice");
    VkPhysicalDeviceIDProperties verified_id_properties{};
    verified_id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 verified_properties2{};
    verified_properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    verified_properties2.pNext = &verified_id_properties;
    vkGetPhysicalDeviceProperties2(physical_, &verified_properties2);
    runtime_vulkan_device_identity verified_device_identity{
        .api_version = verified_properties2.properties.apiVersion,
        .driver_version = verified_properties2.properties.driverVersion,
        .vendor_id = verified_properties2.properties.vendorID,
        .device_id = verified_properties2.properties.deviceID,
    };
    std::copy(std::begin(verified_id_properties.deviceUUID),
              std::end(verified_id_properties.deviceUUID),
              verified_device_identity.device_uuid.begin());
    std::copy(std::begin(verified_properties2.properties.pipelineCacheUUID),
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
        rollback_metal_pipeline_archive_interpose_install(metal_pipeline_archive_interpose_state());
#endif
        throw vulkan_runtime_error("Vulkan physical device identity changed across vkCreateDevice");
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
    std::copy(std::begin(id_properties.deviceUUID),
              std::end(id_properties.deviceUUID),
              features_.device_uuid.begin());
    features_.device_type = static_cast<std::uint32_t>(props.deviceType);
    features_.api_version = props.apiVersion;
    features_.driver_version = props.driverVersion;
    features_.vendor_id = props.vendorID;
    features_.device_id = props.deviceID;
    std::copy(std::begin(props.pipelineCacheUUID),
              std::end(props.pipelineCacheUUID),
              features_.pipeline_cache_uuid.begin());
    features_.software_device = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    features_.max_workgroup_invocations = props.limits.maxComputeWorkGroupInvocations;
    features_.max_workgroup_size_x = props.limits.maxComputeWorkGroupSize[0];
    features_.max_workgroup_size_y = props.limits.maxComputeWorkGroupSize[1];
    features_.max_workgroup_size_z = props.limits.maxComputeWorkGroupSize[2];
    features_.max_workgroup_count_x = props.limits.maxComputeWorkGroupCount[0];
    features_.min_storage_buffer_offset_alignment = props.limits.minStorageBufferOffsetAlignment;
    features_.max_shared_memory_bytes = props.limits.maxComputeSharedMemorySize;
    features_.max_storage_buffer_range = props.limits.maxStorageBufferRange;

    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props);
    std::uint64_t device_local = 0;
    for (std::uint32_t heap_index = 0; heap_index < mem_props.memoryHeapCount; ++heap_index) {
        if (mem_props.memoryHeaps[heap_index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            device_local = std::max(device_local, mem_props.memoryHeaps[heap_index].size);
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

    std::array<VkCommandBuffer, k_rank_prefilter_replay_route_count + 1u> command_buffers{};
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = command_pool_;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = static_cast<std::uint32_t>(command_buffers.size());
    check_vk(vkAllocateCommandBuffers(device_, &cmd_alloc, command_buffers.data()),
             "vkAllocateCommandBuffers");
    command_buffer_ = command_buffers[0];
    for (std::size_t replay_index = 0u; replay_index < prefilter_recorded_commands_.size();
         ++replay_index) {
        prefilter_recorded_commands_[replay_index].command_buffer =
            command_buffers[replay_index + 1u];
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check_vk(vkCreateFence(device_, &fence_info, nullptr, &fence_), "vkCreateFence");

    // --- Fixed descriptor-set layout (4 storage buffers) + pipeline layout ---
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (std::uint32_t binding_index = 0; binding_index < 4; ++binding_index) {
        bindings[binding_index].binding = binding_index;
        bindings[binding_index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[binding_index].descriptorCount = 1;
        bindings[binding_index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    set_layout_info.pBindings = bindings.data();
    check_vk(
        vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr, &descriptor_set_layout_),
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
        gpu_anchor_sort_capacity_ =
            compute_seed_anchor_bitonic_capacity(features_.max_workgroup_invocations,
                                                 features_.max_workgroup_size_x,
                                                 features_.max_shared_memory_bytes);
    }

    features_.available = true;
    std::strncpy(features_.device_name.data(), props.deviceName, features_.device_name.size() - 1u);
    features_.device_name.back() = '\0';
    init_error_.clear();
    bioseq_log<logging_level::debug>(
        "[vulkan_runtime] device ready: {} (maxWGInvocations={}, "
        "maxWGSize={}x{}x{}, maxWGCountX={}, maxSharedMem={}B, "
        "maxStorageBufferRange={}B, scalarBlockLayout={}, softwareDevice={})",
        props.deviceName,
        features_.max_workgroup_invocations,
        features_.max_workgroup_size_x,
        features_.max_workgroup_size_y,
        features_.max_workgroup_size_z,
        features_.max_workgroup_count_x,
        features_.max_shared_memory_bytes,
        features_.max_storage_buffer_range,
        features_.scalar_block_layout,
        features_.software_device);
}

/// @brief Destroys buffers, pipelines, synchronization objects, and device handles.
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
        if (descriptor_pool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        if (pipeline_layout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (descriptor_set_layout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
        if (fence_ != VK_NULL_HANDLE)
            vkDestroyFence(device_, fence_, nullptr);
        if (command_pool_ != VK_NULL_HANDLE)
            vkDestroyCommandPool(device_, command_pool_, nullptr);
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
    rollback_metal_pipeline_archive_interpose_install(metal_pipeline_archive_interpose_state());
#endif
}

/// @brief Selects a memory type satisfying the requested property mask.
/// @param[in] type_bits Bit mask of memory types accepted by the allocation.
/// @param[in] flags Properties every selected memory type must provide.
/// @return Index into the physical device's memory-type array.
/// @throws vulkan_runtime_error When no compatible type exists.
std::uint32_t runtime::find_memory_type(std::uint32_t type_bits,
                                        VkMemoryPropertyFlags flags) const {
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

/// @brief Creates one persistently mapped host-visible buffer.
/// @param[in] bytes Logical buffer size in bytes. Zero requests allocate one byte.
/// @param[in] usage Vulkan usages enabled for the buffer.
/// @return Move-only allocation owned by this runtime and released by `buffer::reset`.
/// @throws vulkan_runtime_error When creation, allocation, binding, or mapping fails.
buffer runtime::create_host_buffer(std::uint64_t bytes, VkBufferUsageFlags usage) {
    if (!features_.available) {
        throw vulkan_runtime_error("create_host_buffer on unavailable device");
    }
    if (bytes == 0)
        bytes = 1; // zero-sized VkBuffers are invalid

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = bytes;
    buf_info.usage = usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer_handle = VK_NULL_HANDLE;
    check_vk(vkCreateBuffer(device_, &buf_info, nullptr, &buffer_handle), "vkCreateBuffer(host)");

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, buffer_handle, &reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = find_memory_type(reqs.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
        (selected_properties &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
#else
        false;
#endif
    return buffer{this,
                  buffer_handle,
                  memory,
                  bytes,
                  reqs.size,
                  selected_properties,
                  footprint_reflected,
                  mapped};
}

/// @brief Queries physical allocation bytes for a logical buffer shape.
/// @param[in] bytes Logical buffer size in bytes. Zero is queried as one byte.
/// @param[in] usage Vulkan usages that affect the memory requirements.
/// @return Device-reported allocation size in bytes.
std::uint64_t runtime::buffer_memory_requirement_locked(std::uint64_t bytes,
                                                        VkBufferUsageFlags usage) const {
    if (bytes == 0u)
        bytes = 1u;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    check_vk(vkCreateBuffer(device_, &info, nullptr, &buffer), "vkCreateBuffer(requirements)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer, &requirements);
    vkDestroyBuffer(device_, buffer, nullptr);
    return requirements.size;
}

/// @brief Totals physical bytes retained by the shared prefilter workspace.
std::uint64_t runtime::prefilter_workspace_allocation_bytes_locked() const noexcept {
    using bioseq::common::saturating_add;
    return saturating_add(
        prefilter_query_buffer_.allocation_bytes,
        saturating_add(prefilter_target_buffer_.allocation_bytes,
                       saturating_add(prefilter_jobs_buffer_.allocation_bytes,
                                      prefilter_results_buffer_.allocation_bytes)));
}

/// @brief Releases every shared prefilter buffer and its admission permit.
void runtime::release_prefilter_workspace_locked() noexcept {
    prefilter_results_buffer_.reset();
    prefilter_jobs_buffer_.reset();
    prefilter_target_buffer_.reset();
    prefilter_query_buffer_.reset();
    prefilter_descriptor_binding_cache_ = {};
    prefilter_workspace_permit_.release();
}

/// @brief Grows one prefilter allocation without shrinking a sufficient buffer.
/// @param[in,out] allocation Existing allocation replaced only when too small.
/// @param[in] bytes Minimum logical size in bytes.
/// @param[in] usage Vulkan usage flags for a replacement allocation.
/// @throws vulkan_runtime_error When admission or allocation fails.
void runtime::ensure_prefilter_workspace_buffer_locked(buffer& allocation,
                                                       std::uint64_t bytes,
                                                       VkBufferUsageFlags usage) {
    if (allocation.is_valid() && allocation.bytes >= bytes)
        return;

    using bioseq::common::saturating_add;
    const std::uint64_t allocation_requirement = buffer_memory_requirement_locked(bytes, usage);
    const std::uint64_t peak =
        saturating_add(prefilter_workspace_allocation_bytes_locked(), allocation_requirement);
    if (peak == (std::numeric_limits<std::uint64_t>::max)()) {
        throw vulkan_runtime_error("rank-prefilter workspace admission estimate saturated");
    }

    bioseq::parallelism::memory_admission_failure failure{};
    if (!prefilter_workspace_permit_) {
        auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            peak, &failure);
        if (!permit) {
            throw vulkan_runtime_error(fmt::format(
                "rank-prefilter workspace admission denied: reason={} requested={}",
                bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                failure.requested_bytes));
        }
        prefilter_workspace_permit_ = std::move(*permit);
    } else if (!prefilter_workspace_permit_.grow_to(peak, &failure)) {
        throw vulkan_runtime_error(
            fmt::format("rank-prefilter workspace growth denied: reason={} requested={}",
                        bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                        failure.requested_bytes));
    }

    try {
        buffer replacement = create_device_buffer(bytes, usage);
        if (replacement.mapped == nullptr) {
            throw vulkan_runtime_error("rank-prefilter workspace allocation is not host-mapped");
        }
        if (replacement.allocation_bytes != allocation_requirement) {
            throw vulkan_runtime_error("rank-prefilter workspace allocation requirement changed");
        }
        allocation = std::move(replacement);
    } catch (...) {
        const std::uint64_t retained = prefilter_workspace_allocation_bytes_locked();
        if (retained <= prefilter_workspace_permit_.bytes()) {
            prefilter_workspace_permit_.shrink_to(retained);
            if (retained == 0u)
                prefilter_workspace_permit_.release();
        }
        throw;
    }

    const std::uint64_t retained = prefilter_workspace_allocation_bytes_locked();
    if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
        retained > prefilter_workspace_permit_.bytes()) {
        release_prefilter_workspace_locked();
        throw vulkan_runtime_error("rank-prefilter workspace exceeded admitted allocation");
    }
    prefilter_workspace_permit_.shrink_to(retained);
}

/// @brief Computes physical arena bytes including retained high-water capacity.
/// @param[in] requested_bytes Logical arena size in bytes.
/// @return Larger of the requested allocation requirement and retained arena allocation.
std::uint64_t runtime::gpu_arena_allocation_requirement(std::uint64_t requested_bytes) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("gpu_arena_allocation_requirement on unavailable device");
    }
    const std::uint64_t requested_requirement = buffer_memory_requirement_locked(
        requested_bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // The GPU frontend arena is persistent.  A smaller later case reuses the prior
    // high-water allocation, so its count admission must cover that actual live
    // allocation rather than only the current request's VkBuffer size.
    const std::uint64_t current_requirement =
        gpu_arena_ ? gpu_arena_->device_buffer().allocation_bytes : 0u;
    return std::max(requested_requirement, current_requirement);
}

/// @brief Computes physical allocation bytes for one buffer usage contract.
/// @param[in] requested_bytes Logical buffer size in bytes.
/// @param[in] usage Vulkan usage flags for the proposed buffer.
/// @return Device-reported allocation size in bytes.
std::uint64_t runtime::buffer_allocation_requirement(std::uint64_t requested_bytes,
                                                     VkBufferUsageFlags usage) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("buffer_allocation_requirement on unavailable device");
    }
    return buffer_memory_requirement_locked(requested_bytes, usage);
}

/// @brief Creates one device buffer admitted against its physical allocation.
/// @param[in] bytes Logical buffer size in bytes. Zero requests allocate one byte.
/// @param[in] usage Vulkan usages enabled for the buffer.
/// @return Move-only runtime-owned allocation, persistently mapped when supported.
/// @throws vulkan_runtime_error When allocation or binding fails.
buffer runtime::create_device_buffer(std::uint64_t bytes, VkBufferUsageFlags usage) {
    if (!features_.available) {
        throw vulkan_runtime_error("create_device_buffer on unavailable device");
    }
    if (bytes == 0)
        bytes = 1;

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = bytes;
    buf_info.usage = usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer_handle = VK_NULL_HANDLE;
    check_vk(vkCreateBuffer(device_, &buf_info, nullptr, &buffer_handle), "vkCreateBuffer(device)");

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
        mem_type = find_memory_type(reqs.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        found = true;
    } catch (const vulkan_runtime_error&) {
        found = false;
    }
    if (!found) {
        mem_type = find_memory_type(reqs.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
        (selected_properties &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
#else
        false;
#endif
    return buffer{this,
                  buffer_handle,
                  memory,
                  bytes,
                  reqs.size,
                  selected_properties,
                  footprint_reflected,
                  mapped};
}

/// @brief Destroys the Vulkan objects held by one runtime-owned allocation.
/// @param[in,out] allocation Allocation whose mapped memory, buffer, and memory are released.
void runtime::destroy_buffer(buffer& allocation) noexcept {
    if (device_ == VK_NULL_HANDLE)
        return;
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

} // namespace bioseq::gpu::vulkan

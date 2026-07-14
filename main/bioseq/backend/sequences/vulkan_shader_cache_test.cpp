#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>
#include <vulkan/vulkan.h>

import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.gpu.vulkan.phase_c_abi;

namespace {

namespace gv = bioseq::gpu::vulkan;

bool shader_module_create_called = false;

VKAPI_ATTR VkResult VKAPI_CALL record_shader_module_create(
    VkDevice,
    const VkShaderModuleCreateInfo*,
    const VkAllocationCallbacks*,
    VkShaderModule*)
{
    shader_module_create_called = true;
    return VK_SUCCESS;
}

struct NoPublishCompileService {
    int calls = 0;

    [[nodiscard]] std::expected<void, std::string> ensure_variant_compiled(
        gv::ShaderVariantKey)
    {
        ++calls;
        return {};
    }
};

struct TempDir {
    std::filesystem::path path;

    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
             / ("bioseq_shader_cache_test_" + std::to_string(::getpid())
                + "_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

[[nodiscard]] bool contains_key(
    const std::vector<gv::ShaderVariantKey>& keys,
    gv::ShaderVariantKey needle)
{
    return std::find(keys.begin(), keys.end(), needle) != keys.end();
}

[[nodiscard]] bool is_hex_digest(std::string_view digest)
{
    return digest.size() == 16
        && std::all_of(digest.begin(), digest.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text)
{
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

[[nodiscard]] std::size_t spirv_opcode_count(
    std::span<const std::uint32_t> words,
    std::uint16_t opcode) noexcept
{
    constexpr std::uint32_t spirv_magic = 0x07230203u;
    if (words.size() < 5u || words.front() != spirv_magic) return 0u;
    std::size_t count = 0u;
    for (std::size_t offset = 5u; offset < words.size();) {
        const std::uint32_t instruction = words[offset];
        const std::size_t word_count = instruction >> 16u;
        if (word_count == 0u || word_count > words.size() - offset) {
            return 0u;
        }
        if (static_cast<std::uint16_t>(instruction & 0xffffu) == opcode) {
            ++count;
        }
        offset += word_count;
    }
    return count;
}

// Test-only scalar oracles for the inactive ABI. They are deliberately local
// to this unit test and are not a host execution alternative to the GPU route.
[[nodiscard]] std::uint32_t grouped32_partition_validation_oracle(
    const gv::GpuGrouped32PartitionWire& partition,
    std::span<const gv::GpuGrouped32ControlWire> controls,
    std::span<const gv::GpuGrouped32PositionWire> positions)
{
    using Error = gv::GpuGrouped32ValidationError;
    const auto bit = [](Error error) {
        return static_cast<std::uint32_t>(error);
    };
    std::uint32_t status = 0u;
    const bool control_bounds = partition.control_offset <= controls.size()
        && partition.control_count
            <= controls.size() - partition.control_offset;
    const bool position_bounds = partition.position_offset <= positions.size()
        && partition.position_count
            <= positions.size() - partition.position_offset;
    if (!control_bounds || !position_bounds
        || partition.control_count == 0u
        || partition.position_count == 0u) {
        status |= bit(Error::descriptor_bounds);
    }
    if (partition.flags != 0u || partition.reserved0 != 0u
        || partition.reserved1 != 0u) {
        status |= bit(Error::descriptor_identity);
    }
    if (partition.segment_base_bp >= partition.segment_end_bp
        || partition.segment_end_bp - partition.segment_base_bp
            > std::uint64_t{gv::k_gpu_grouped32_max_local_bp} + 1u
        || partition.low_hash > partition.high_hash) {
        status |= bit(Error::segment_bounds);
    }
    if (!control_bounds || !position_bounds
        || partition.control_count == 0u
        || partition.position_count == 0u
        || partition.segment_base_bp >= partition.segment_end_bp) {
        return status;
    }

    std::uint32_t expected_position_offset = 0u;
    std::optional<std::uint32_t> previous_hash;
    for (std::uint32_t local_control = 0u;
         local_control < partition.control_count; ++local_control) {
        const auto& control = controls[partition.control_offset + local_control];
        if ((previous_hash && control.hash <= *previous_hash)
            || control.hash < partition.low_hash
            || control.hash > partition.high_hash) {
            status |= bit(Error::control_order);
        }
        if (control.position_count == 0u
            || control.position_offset != expected_position_offset
            || control.position_offset > partition.position_count
            || control.position_count
                > partition.position_count - control.position_offset) {
            status |= bit(Error::control_positions);
            continue;
        }
        previous_hash = control.hash;
        expected_position_offset =
            control.position_offset + control.position_count;
        std::optional<std::uint32_t> previous_position;
        for (std::uint32_t local_position = 0u;
             local_position < control.position_count; ++local_position) {
            const std::uint32_t encoded = positions[
                partition.position_offset + control.position_offset
                + local_position].local_bp_strand;
            if (std::uint64_t{encoded >> 1u}
                >= partition.segment_end_bp - partition.segment_base_bp) {
                status |= bit(Error::position_bounds);
            }
            if (previous_position && encoded <= *previous_position) {
                status |= bit(Error::position_order);
            }
            previous_position = encoded;
        }
    }
    if (!previous_hash
        || controls[partition.control_offset].hash != partition.low_hash
        || *previous_hash != partition.high_hash
        || expected_position_offset != partition.position_count) {
        status |= bit(Error::control_positions);
    }
    return status;
}

struct Grouped32GlobalPositionOracle {
    std::uint64_t bp = 0;
    std::uint32_t strand = 0;
};

[[nodiscard]] std::optional<Grouped32GlobalPositionOracle>
grouped32_globalize_oracle(
    const gv::GpuGrouped32PartitionWire& partition,
    const gv::GpuGrouped32PositionWire position)
{
    if (partition.segment_base_bp >= partition.segment_end_bp) {
        return std::nullopt;
    }
    const std::uint32_t local_bp = position.local_bp_strand >> 1u;
    if (std::uint64_t{local_bp}
        >= partition.segment_end_bp - partition.segment_base_bp) {
        return std::nullopt;
    }
    return Grouped32GlobalPositionOracle{
        .bp = partition.segment_base_bp + local_bp,
        .strand = position.local_bp_strand & 1u,
    };
}

[[nodiscard]] constexpr std::uint32_t grouped32_saturating_count_oracle(
    std::uint32_t current,
    std::uint32_t increment,
    std::uint32_t max_occ) noexcept
{
    const std::uint32_t ceiling =
        max_occ == (std::numeric_limits<std::uint32_t>::max)()
        ? max_occ : max_occ + 1u;
    if (current >= ceiling) return ceiling;
    return current + std::min(increment, ceiling - current);
}

[[nodiscard]] constexpr std::uint32_t grouped32_finalize_count_oracle(
    std::uint32_t global_count,
    std::uint32_t max_occ) noexcept
{
    return global_count <= max_occ ? global_count : 0u;
}

[[nodiscard]] std::uint32_t grouped32_directory_identity_oracle(
    std::span<const gv::GpuGrouped32PartitionWire> partitions)
{
    const auto bit = [](gv::GpuGrouped32ValidationError error) {
        return static_cast<std::uint32_t>(error);
    };
    std::uint32_t status = 0u;
    for (std::size_t ordinal = 0; ordinal < partitions.size(); ++ordinal) {
        const auto& partition = partitions[ordinal];
        if (partition.directory_ordinal != static_cast<std::uint32_t>(ordinal)
            || (ordinal != 0u
                && partition.partition_id
                    <= partitions[ordinal - 1u].partition_id)) {
            status |= bit(gv::GpuGrouped32ValidationError::descriptor_identity);
        }
    }
    return status;
}

[[nodiscard]] bool grouped32_query_encoding_oracle(
    const gv::GpuGrouped32QueryRecordWire& record,
    const gv::GpuGrouped32QueryMinimizerWire& minimizer,
    std::uint32_t kmer_size = 15u)
{
    const std::uint32_t q_last = minimizer.query_bp_strand >> 1u;
    return kmer_size == 15u && record.query_length <= 0x80000000u
        && record.query_length >= kmer_size && q_last < record.query_length
        && q_last + 1u >= kmer_size;
}

[[nodiscard]] constexpr bool grouped32_gate_oracle(
    const gv::GpuGrouped32ValidationStateWire& validation,
    const gv::GpuGrouped32QueryAdapterStateWire& adapter) noexcept
{
    using Completion = gv::GpuGrouped32ValidationCompletion;
    return validation.completion
            == static_cast<std::uint32_t>(Completion::accepted)
        && validation.status == 0u
        && adapter.completion
            == static_cast<std::uint32_t>(Completion::accepted)
        && adapter.status == 0u;
}

template <class T>
[[nodiscard]] std::vector<std::byte> pod_payload(std::span<const T> values)
{
    const auto bytes = std::as_bytes(values);
    return {bytes.begin(), bytes.end()};
}

template <class T>
[[nodiscard]] std::vector<std::byte> pod_payload(const T& value)
{
    return pod_payload<T>(std::span<const T>{&value, 1u});
}

template <class T>
[[nodiscard]] T pod_from_payload(
    const std::vector<std::byte>& payload,
    std::size_t byte_offset = 0u)
{
    REQUIRE(byte_offset <= payload.size());
    REQUIRE(sizeof(T) <= payload.size() - byte_offset);
    T value{};
    std::memcpy(&value, payload.data() + byte_offset, sizeof(T));
    return value;
}

/// Test-only raw Vulkan dispatch harness. It requires a non-CPU Vulkan device
/// (hardware Vulkan or a hardware-backed translation layer such as MoltenVK)
/// and never constructs a backend proof.
class Grouped32ValidationHarness {
public:
    struct RunResult {
        std::vector<std::vector<std::byte>> bindings;
        bool guards_intact = false;
    };

    struct DispatchMetric {
        gv::ShaderKind kind{};
        std::uint64_t dispatch_calls = 0u;
        std::uint64_t workgroups_x = 0u;
    };

    [[nodiscard]] std::uint64_t dispatch_calls(
        gv::ShaderKind kind) const noexcept
    {
        for (const auto& metric : dispatch_metrics_) {
            if (metric.kind == kind) return metric.dispatch_calls;
        }
        return 0u;
    }

    [[nodiscard]] std::uint64_t dispatched_workgroups_x(
        gv::ShaderKind kind) const noexcept
    {
        for (const auto& metric : dispatch_metrics_) {
            if (metric.kind == kind) return metric.workgroups_x;
        }
        return 0u;
    }

    [[nodiscard]] std::uint64_t total_dispatch_calls() const noexcept
    {
        std::uint64_t total = 0u;
        for (const auto& metric : dispatch_metrics_) {
            total += metric.dispatch_calls;
        }
        return total;
    }

    [[nodiscard]] std::uint64_t total_dispatched_workgroups_x() const noexcept
    {
        std::uint64_t total = 0u;
        for (const auto& metric : dispatch_metrics_) {
            total += metric.workgroups_x;
        }
        return total;
    }

    [[nodiscard]] std::string_view device_name() const noexcept
    {
        return physical_properties_.deviceName;
    }

    [[nodiscard]] VkPhysicalDeviceType device_type() const noexcept
    {
        return physical_properties_.deviceType;
    }

    Grouped32ValidationHarness() try
    {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "bioseq-grouped32-validation-test";
        app.apiVersion = VK_API_VERSION_1_2;

        std::vector<const char*> instance_extensions;
        VkInstanceCreateFlags instance_flags = 0u;
#if defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
        std::uint32_t extension_count = 0u;
        if (vkEnumerateInstanceExtensionProperties(
                nullptr, &extension_count, nullptr) == VK_SUCCESS) {
            std::vector<VkExtensionProperties> extensions(extension_count);
            if (vkEnumerateInstanceExtensionProperties(
                    nullptr, &extension_count, extensions.data()) == VK_SUCCESS) {
                for (const auto& extension : extensions) {
                    if (std::string_view{extension.extensionName}
                        == VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) {
                        instance_extensions.push_back(
                            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                        instance_flags |=
                            VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
                        break;
                    }
                }
            }
        }
#endif
        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.flags = instance_flags;
        instance_info.pApplicationInfo = &app;
        instance_info.enabledExtensionCount =
            static_cast<std::uint32_t>(instance_extensions.size());
        instance_info.ppEnabledExtensionNames = instance_extensions.data();
        check(vkCreateInstance(&instance_info, nullptr, &instance_),
              "vkCreateInstance");

        std::uint32_t physical_count = 0u;
        check(vkEnumeratePhysicalDevices(instance_, &physical_count, nullptr),
              "vkEnumeratePhysicalDevices(count)");
        if (physical_count == 0u) {
            throw std::runtime_error("no Vulkan physical device");
        }
        std::vector<VkPhysicalDevice> physicals(physical_count);
        check(vkEnumeratePhysicalDevices(
                  instance_, &physical_count, physicals.data()),
              "vkEnumeratePhysicalDevices(list)");
        for (const VkPhysicalDevice candidate : physicals) {
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            if (features.shaderInt64 == VK_FALSE) continue;
            VkPhysicalDeviceProperties candidate_properties{};
            vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
            if (candidate_properties.deviceType
                == VK_PHYSICAL_DEVICE_TYPE_CPU) {
                continue;
            }
            std::uint32_t family_count = 0u;
            vkGetPhysicalDeviceQueueFamilyProperties(
                candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(
                candidate, &family_count, families.data());
            for (std::uint32_t i = 0u; i < family_count; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0u) {
                    physical_ = candidate;
                    queue_family_ = i;
                    enabled_features_.shaderInt64 = VK_TRUE;
                    break;
                }
            }
            if (physical_ != VK_NULL_HANDLE) break;
        }
        if (physical_ == VK_NULL_HANDLE) {
            throw std::runtime_error(
                "no non-CPU Vulkan compute device with shaderInt64");
        }
        vkGetPhysicalDeviceProperties(physical_, &physical_properties_);
        vkGetPhysicalDeviceMemoryProperties(physical_, &memory_properties_);

        std::vector<const char*> device_extensions;
#if defined(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)
        std::uint32_t device_extension_count = 0u;
        if (vkEnumerateDeviceExtensionProperties(
                physical_, nullptr, &device_extension_count, nullptr)
            == VK_SUCCESS) {
            std::vector<VkExtensionProperties> extensions(
                device_extension_count);
            if (vkEnumerateDeviceExtensionProperties(
                    physical_, nullptr, &device_extension_count,
                    extensions.data()) == VK_SUCCESS) {
                for (const auto& extension : extensions) {
                    if (std::string_view{extension.extensionName}
                        == VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME) {
                        device_extensions.push_back(
                            VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
                        break;
                    }
                }
            }
        }
#endif
        constexpr float priority = 1.0F;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1u;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1u;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.pEnabledFeatures = &enabled_features_;
        device_info.enabledExtensionCount =
            static_cast<std::uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.data();
        check(vkCreateDevice(physical_, &device_info, nullptr, &device_),
              "vkCreateDevice");
        vkGetDeviceQueue(device_, queue_family_, 0u, &queue_);

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family_;
        check(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_),
              "vkCreateCommandPool");
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(device_, &fence_info, nullptr, &fence_),
              "vkCreateFence");
    } catch (...) {
        if (fence_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, fence_, nullptr);
        }
        if (command_pool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
        }
        if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
        throw;
    }

    Grouped32ValidationHarness(const Grouped32ValidationHarness&) = delete;
    Grouped32ValidationHarness& operator=(
        const Grouped32ValidationHarness&) = delete;

    ~Grouped32ValidationHarness()
    {
        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, fence_, nullptr);
        }
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
        }
        if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
    }

    [[nodiscard]] RunResult run(
        gv::ShaderKind kind,
        std::span<const std::byte> push,
        std::vector<std::vector<std::byte>> payloads,
        std::uint32_t group_count_x = 1u)
    {
        if (group_count_x == 0u) {
            throw std::runtime_error(
                "grouped32 test dispatch must execute at least one workgroup");
        }
        const auto key = gv::make_shader_key(kind, 384u, 128u, 2u);
        const std::vector<std::uint32_t>* spirv = nullptr;
        for (const auto& cached : shader_cache_) {
            if (cached.first == kind) {
                spirv = &cached.second;
                break;
            }
        }
        if (spirv == nullptr) {
            shader_cache_.emplace_back(
                kind, gv::compile_shader_spirv(key.to_request()));
            spirv = &shader_cache_.back().second;
        }
        const std::uint32_t binding_count = gv::descriptor_binding_count(
            gv::descriptor_layout_for(kind));
        if (payloads.size() != binding_count
            || push.size() != gv::push_constant_size(kind)) {
            throw std::runtime_error("grouped32 test dispatch ABI mismatch");
        }

        RunResources resources{device_, command_pool_};
        std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
        layout_bindings.reserve(binding_count);
        for (std::uint32_t binding = 0u; binding < binding_count; ++binding) {
            VkDescriptorSetLayoutBinding layout_binding{};
            layout_binding.binding = binding;
            layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            layout_binding.descriptorCount = 1u;
            layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            layout_bindings.push_back(layout_binding);
        }
        VkDescriptorSetLayoutCreateInfo set_layout_info{};
        set_layout_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_layout_info.bindingCount = binding_count;
        set_layout_info.pBindings = layout_bindings.data();
        check(vkCreateDescriptorSetLayout(
                  device_, &set_layout_info, nullptr, &resources.set_layout),
              "vkCreateDescriptorSetLayout");

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0u;
        push_range.size = static_cast<std::uint32_t>(push.size());
        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1u;
        pipeline_layout_info.pSetLayouts = &resources.set_layout;
        pipeline_layout_info.pushConstantRangeCount = push.empty() ? 0u : 1u;
        pipeline_layout_info.pPushConstantRanges =
            push.empty() ? nullptr : &push_range;
        check(vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
                                     &resources.pipeline_layout),
              "vkCreatePipelineLayout");

        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = spirv->size() * sizeof(std::uint32_t);
        shader_info.pCode = spirv->data();
        check(vkCreateShaderModule(
                  device_, &shader_info, nullptr, &resources.shader),
              "vkCreateShaderModule");
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = resources.shader;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = resources.pipeline_layout;
        check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1u,
                                       &pipeline_info, nullptr,
                                       &resources.pipeline),
              "vkCreateComputePipelines");

        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_size.descriptorCount = binding_count;
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1u;
        pool_info.poolSizeCount = 1u;
        pool_info.pPoolSizes = &pool_size;
        check(vkCreateDescriptorPool(
                  device_, &pool_info, nullptr, &resources.descriptor_pool),
              "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = resources.descriptor_pool;
        allocate_info.descriptorSetCount = 1u;
        allocate_info.pSetLayouts = &resources.set_layout;
        check(vkAllocateDescriptorSets(
                  device_, &allocate_info, &resources.descriptor_set),
              "vkAllocateDescriptorSets");

        resources.buffers.reserve(binding_count);
        for (const auto& payload : payloads) {
            if (payload.empty()) {
                throw std::runtime_error(
                    "grouped32 test descriptors must have non-zero ranges");
            }
            resources.buffers.push_back(create_buffer(payload));
        }
        std::vector<VkDescriptorBufferInfo> buffer_infos(binding_count);
        std::vector<VkWriteDescriptorSet> writes(binding_count);
        for (std::uint32_t binding = 0u; binding < binding_count; ++binding) {
            buffer_infos[binding].buffer = resources.buffers[binding].buffer;
            buffer_infos[binding].offset =
                resources.buffers[binding].descriptor_offset;
            buffer_infos[binding].range =
                resources.buffers[binding].descriptor_bytes;
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = resources.descriptor_set;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1u;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo = &buffer_infos[binding];
        }
        vkUpdateDescriptorSets(
            device_, binding_count, writes.data(), 0u, nullptr);

        VkCommandBufferAllocateInfo command_allocate{};
        command_allocate.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_allocate.commandPool = command_pool_;
        command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocate.commandBufferCount = 1u;
        check(vkAllocateCommandBuffers(
                  device_, &command_allocate, &resources.command_buffer),
              "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(resources.command_buffer, &begin),
              "vkBeginCommandBuffer");
        VkMemoryBarrier device_barrier{};
        device_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        device_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        device_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
            | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            resources.command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &device_barrier,
            0u, nullptr, 0u, nullptr);
        vkCmdBindPipeline(resources.command_buffer,
                          VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipeline);
        vkCmdBindDescriptorSets(
            resources.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            resources.pipeline_layout, 0u, 1u, &resources.descriptor_set,
            0u, nullptr);
        if (!push.empty()) {
            vkCmdPushConstants(resources.command_buffer,
                               resources.pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                               static_cast<std::uint32_t>(push.size()),
                               push.data());
        }
        vkCmdDispatch(resources.command_buffer, group_count_x, 1u, 1u);
        VkMemoryBarrier host_barrier{};
        host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(
            resources.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0u, 1u, &host_barrier,
            0u, nullptr, 0u, nullptr);
        check(vkEndCommandBuffer(resources.command_buffer),
              "vkEndCommandBuffer");
        check(vkResetFences(device_, 1u, &fence_), "vkResetFences");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1u;
        submit.pCommandBuffers = &resources.command_buffer;
        check(vkQueueSubmit(queue_, 1u, &submit, fence_), "vkQueueSubmit");
        check(vkWaitForFences(
                  device_, 1u, &fence_, VK_TRUE, 10'000'000'000ULL),
              "vkWaitForFences");

        auto metric = std::find_if(
            dispatch_metrics_.begin(), dispatch_metrics_.end(),
            [kind](const DispatchMetric& candidate) {
                return candidate.kind == kind;
            });
        if (metric == dispatch_metrics_.end()) {
            dispatch_metrics_.push_back(DispatchMetric{.kind = kind});
            metric = std::prev(dispatch_metrics_.end());
        }
        ++metric->dispatch_calls;
        metric->workgroups_x += group_count_x;

        RunResult result{};
        result.guards_intact = true;
        result.bindings.reserve(binding_count);
        for (auto& buffer : resources.buffers) {
            buffer.invalidate();
            std::vector<std::byte> output(
                static_cast<std::size_t>(buffer.descriptor_bytes));
            const auto* bytes = static_cast<const std::byte*>(buffer.mapped);
            std::memcpy(output.data(),
                        bytes + static_cast<std::size_t>(
                            buffer.descriptor_offset),
                        output.size());
            result.bindings.push_back(std::move(output));
            for (std::size_t i = 0u;
                 i < static_cast<std::size_t>(buffer.descriptor_offset); ++i) {
                if (bytes[i] != k_guard) result.guards_intact = false;
            }
            for (std::size_t i = static_cast<std::size_t>(
                     buffer.descriptor_offset + buffer.descriptor_bytes);
                 i < static_cast<std::size_t>(buffer.allocation_bytes); ++i) {
                if (bytes[i] != k_guard) result.guards_intact = false;
            }
        }
        return result;
    }

private:
    static inline constexpr std::byte k_guard{0xa5};
    // Every binding is surrounded by a guard at least as wide as the largest
    // element in either raw-harness descriptor layout
    // (GpuGrouped32PartitionWire, 64 bytes). The prefix is additionally
    // rounded up to the device's descriptor-offset alignment.
    static inline constexpr VkDeviceSize k_guard_bytes = 64u;

    static void check(VkResult result, std::string_view operation)
    {
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                std::string(operation) + " failed: "
                + std::to_string(static_cast<int>(result)));
        }
    }

    struct OwnedBuffer {
        VkDevice device = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize descriptor_offset = 0u;
        VkDeviceSize descriptor_bytes = 0u;
        VkDeviceSize allocation_bytes = 0u;
        bool coherent = false;

        OwnedBuffer() = default;
        OwnedBuffer(const OwnedBuffer&) = delete;
        OwnedBuffer& operator=(const OwnedBuffer&) = delete;
        OwnedBuffer(OwnedBuffer&& source) noexcept { *this = std::move(source); }
        OwnedBuffer& operator=(OwnedBuffer&& source) noexcept
        {
            if (this != &source) {
                reset();
                device = std::exchange(source.device, VK_NULL_HANDLE);
                memory = std::exchange(source.memory, VK_NULL_HANDLE);
                buffer = std::exchange(source.buffer, VK_NULL_HANDLE);
                mapped = std::exchange(source.mapped, nullptr);
                descriptor_offset =
                    std::exchange(source.descriptor_offset, 0u);
                descriptor_bytes = std::exchange(source.descriptor_bytes, 0u);
                allocation_bytes = std::exchange(source.allocation_bytes, 0u);
                coherent = std::exchange(source.coherent, false);
            }
            return *this;
        }
        ~OwnedBuffer() { reset(); }

        void invalidate()
        {
            if (!coherent) {
                VkMappedMemoryRange range{};
                range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = memory;
                range.offset = 0u;
                range.size = VK_WHOLE_SIZE;
                check(vkInvalidateMappedMemoryRanges(device, 1u, &range),
                      "vkInvalidateMappedMemoryRanges");
            }
        }
        void reset() noexcept
        {
            if (mapped != nullptr && memory != VK_NULL_HANDLE) {
                vkUnmapMemory(device, memory);
            }
            if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
            if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
            mapped = nullptr;
            buffer = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
        }
    };

    struct RunResources {
        VkDevice device = VK_NULL_HANDLE;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkShaderModule shader = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        std::vector<OwnedBuffer> buffers;

        RunResources(VkDevice value, VkCommandPool pool)
            : device(value), command_pool(pool) {}
        ~RunResources()
        {
            if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
            if (command_buffer != VK_NULL_HANDLE
                && command_pool != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(
                    device, command_pool, 1u, &command_buffer);
            }
            buffers.clear();
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, pipeline, nullptr);
            }
            if (shader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device, shader, nullptr);
            }
            if (pipeline_layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            }
            if (descriptor_pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            }
            if (set_layout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            }
        }
    };

    [[nodiscard]] OwnedBuffer create_buffer(
        const std::vector<std::byte>& payload)
    {
        OwnedBuffer result{};
        result.device = device_;
        const VkDeviceSize descriptor_alignment = std::max<VkDeviceSize>(
            1u,
            physical_properties_.limits.minStorageBufferOffsetAlignment);
        result.descriptor_offset =
            ((k_guard_bytes + descriptor_alignment - 1u)
                / descriptor_alignment)
            * descriptor_alignment;
        result.descriptor_bytes = payload.size();
        result.allocation_bytes = result.descriptor_offset
            + result.descriptor_bytes + k_guard_bytes;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = result.allocation_bytes;
        buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(
                  device_, &buffer_info, nullptr, &result.buffer),
              "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, result.buffer, &requirements);
        std::optional<std::uint32_t> memory_type;
        for (std::uint32_t i = 0u; i < memory_properties_.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) == 0u) continue;
            const auto flags = memory_properties_.memoryTypes[i].propertyFlags;
            if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0u) continue;
            if (!memory_type
                || (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u) {
                memory_type = i;
                result.coherent =
                    (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;
                if (result.coherent) break;
            }
        }
        if (!memory_type) {
            throw std::runtime_error("no host-visible Vulkan memory type");
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *memory_type;
        check(vkAllocateMemory(
                  device_, &allocation, nullptr, &result.memory),
              "vkAllocateMemory");
        check(vkBindBufferMemory(device_, result.buffer, result.memory, 0u),
              "vkBindBufferMemory");
        check(vkMapMemory(device_, result.memory, 0u, VK_WHOLE_SIZE, 0u,
                          &result.mapped),
              "vkMapMemory");
        std::fill_n(
            static_cast<std::byte*>(result.mapped),
            static_cast<std::size_t>(result.allocation_bytes), k_guard);
        std::memcpy(static_cast<std::byte*>(result.mapped)
                        + static_cast<std::size_t>(result.descriptor_offset),
                    payload.data(), payload.size());
        if (!result.coherent) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = result.memory;
            range.offset = 0u;
            range.size = VK_WHOLE_SIZE;
            check(vkFlushMappedMemoryRanges(device_, 1u, &range),
                  "vkFlushMappedMemoryRanges");
        }
        return result;
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physical_properties_{};
    VkPhysicalDeviceFeatures enabled_features_{};
    VkPhysicalDeviceMemoryProperties memory_properties_{};
    std::uint32_t queue_family_ = 0u;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::vector<std::pair<gv::ShaderKind, std::vector<std::uint32_t>>>
        shader_cache_;
    std::vector<DispatchMetric> dispatch_metrics_;
};

struct Grouped32CoreDispatchFixture {
    gv::GpuGrouped32Abi2Push push{};
    std::vector<std::vector<std::byte>> bindings;
};

[[nodiscard]] Grouped32CoreDispatchFixture make_grouped32_core_fixture(
    bool duplicate_position = false,
    std::uint32_t q_last = 14u)
{
    Grouped32CoreDispatchFixture fixture{};
    const gv::GpuGrouped32PartitionWire partition{
        .partition_id = 3u,
        .directory_ordinal = 0u,
        .position_segment = 0u,
        .control_offset = 0u,
        .control_count = 1u,
        .position_offset = 0u,
        .position_count = 2u,
        .low_hash = 9u,
        .high_hash = 9u,
        .segment_base_bp = 0u,
        .segment_end_bp = 100u,
    };
    const gv::GpuGrouped32ControlWire control{
        .hash = 9u, .position_offset = 0u, .position_count = 2u};
    const std::array positions{
        gv::GpuGrouped32PositionWire{.local_bp_strand = 8u},
        gv::GpuGrouped32PositionWire{
            .local_bp_strand = duplicate_position ? 8u : 10u},
    };
    const gv::GpuGrouped32QueryRecordWire record{
        .minimizer_offset = 0u,
        .minimizer_count = 1u,
        .query_length = 100u,
        .query_state_index = 0u,
        .occurrence_offset = 0u,
        .anchor_base = 0u,
        .anchor_capacity = 4u,
        .batch_join_ordinal = (std::uint64_t{1} << 40u) + 7u,
    };
    const gv::GpuGrouped32QueryMinimizerWire minimizer{
        .hash = 9u,
        .query_bp_strand = q_last << 1u,
        .occurrence_index = 0u,
        .query_record_index = 0u,
    };
    const gv::GpuGrouped32OccurrenceStateWire occurrence{};
    const gv::GpuGrouped32QueryStateWire query_state{};
    const std::array<gv::GpuGrouped32GlobalAnchorWire, 4> anchors{};
    gv::GpuGrouped32ValidationStateWire validation{};
    validation.expected_workgroups = 1u;
    validation.completion = static_cast<std::uint32_t>(
        gv::GpuGrouped32ValidationCompletion::pending);
    const std::array<gv::GpuGrouped32DispatchIndirectCommandWire,
        static_cast<std::size_t>(gv::GpuGrouped32IndirectStage::count)>
        commands{};
    const std::array<gv::GpuGrouped32AnchorRadixKeyWire, 4> keys{};
    const gv::GpuGrouped32SortSegmentWire sort_segment{};
    gv::GpuGrouped32QueryAdapterStateWire adapter{};
    adapter.completed_records = 1u;
    adapter.expected_records = 1u;
    adapter.total_minimizers = 1u;
    adapter.max_query_minimizers = 1u;
    adapter.completion = static_cast<std::uint32_t>(
        gv::GpuGrouped32ValidationCompletion::accepted);

    fixture.bindings.resize(13u);
    fixture.bindings[0] = pod_payload(partition);
    fixture.bindings[1] = pod_payload(control);
    fixture.bindings[2] = pod_payload<gv::GpuGrouped32PositionWire>(positions);
    fixture.bindings[3] = pod_payload(record);
    fixture.bindings[4] = pod_payload(minimizer);
    fixture.bindings[5] = pod_payload(occurrence);
    fixture.bindings[6] = pod_payload(query_state);
    fixture.bindings[7] = pod_payload<gv::GpuGrouped32GlobalAnchorWire>(anchors);
    fixture.bindings[8] = pod_payload(validation);
    fixture.bindings[9] = pod_payload<
        gv::GpuGrouped32DispatchIndirectCommandWire>(commands);
    fixture.bindings[10] = pod_payload<
        gv::GpuGrouped32AnchorRadixKeyWire>(keys);
    fixture.bindings[11] = pod_payload(sort_segment);
    fixture.bindings[12] = pod_payload(adapter);

    fixture.push.partition_count = 1u;
    fixture.push.query_record_count = 1u;
    fixture.push.query_minimizer_capacity = 1u;
    fixture.push.occurrence_count = 1u;
    fixture.push.control_count = 1u;
    fixture.push.position_count = 2u;
    fixture.push.anchor_capacity = 4u;
    fixture.push.max_occ = 10u;
    fixture.push.kmer_size = 15u;
    fixture.push.validation_index = 0u;
    fixture.push.validation_state_count = 1u;
    fixture.push.indirect_base = 0u;
    fixture.push.indirect_command_count =
        static_cast<std::uint32_t>(gv::GpuGrouped32IndirectStage::count);
    fixture.push.max_partition_groups_y = 1u;
    fixture.push.max_workgroups_x = 1u;
    fixture.push.adapter_state_index = 0u;
    fixture.push.adapter_state_count = 1u;
    return fixture;
}

[[nodiscard]] Grouped32CoreDispatchFixture
make_grouped32_multiworkgroup_validation_fixture(bool late_failure)
{
    constexpr std::uint32_t record_count = 129u;
    auto fixture = make_grouped32_core_fixture(false, 14u);
    std::vector<gv::GpuGrouped32QueryRecordWire> records(record_count);
    std::vector<gv::GpuGrouped32QueryStateWire> query_states(record_count);
    std::vector<gv::GpuGrouped32SortSegmentWire> sort_segments(record_count);
    for (std::uint32_t index = 0u; index < record_count; ++index) {
        records[index].query_state_index = index;
        records[index].batch_join_ordinal =
            (std::uint64_t{1} << 44u) + index + 1u;
    }
    if (late_failure) records.back().reserved1 = 1u;
    fixture.bindings[3] = pod_payload<gv::GpuGrouped32QueryRecordWire>(
        std::span<const gv::GpuGrouped32QueryRecordWire>{records});
    fixture.bindings[6] = pod_payload<gv::GpuGrouped32QueryStateWire>(
        std::span<const gv::GpuGrouped32QueryStateWire>{query_states});
    fixture.bindings[11] = pod_payload<gv::GpuGrouped32SortSegmentWire>(
        std::span<const gv::GpuGrouped32SortSegmentWire>{sort_segments});

    gv::GpuGrouped32QueryAdapterStateWire adapter{};
    adapter.completed_records = record_count;
    adapter.expected_records = record_count;
    adapter.completion = static_cast<std::uint32_t>(
        gv::GpuGrouped32ValidationCompletion::accepted);
    fixture.bindings[12] = pod_payload(adapter);
    gv::GpuGrouped32ValidationStateWire validation{};
    validation.expected_workgroups = 2u;
    validation.completion = static_cast<std::uint32_t>(
        gv::GpuGrouped32ValidationCompletion::pending);
    fixture.bindings[8] = pod_payload(validation);
    std::array<gv::GpuGrouped32DispatchIndirectCommandWire,
        static_cast<std::size_t>(gv::GpuGrouped32IndirectStage::count)>
        dirty_commands{};
    for (auto& command : dirty_commands) {
        command = {.group_count_x = 77u,
                   .group_count_y = 78u,
                   .group_count_z = 79u};
    }
    fixture.bindings[9] = pod_payload<
        gv::GpuGrouped32DispatchIndirectCommandWire>(dirty_commands);

    fixture.push.query_record_count = record_count;
    fixture.push.max_workgroups_x = 2u;
    return fixture;
}

struct Grouped32AdapterDispatchFixture {
    gv::GpuGrouped32QueryAdapterPush push{};
    std::vector<std::vector<std::byte>> bindings;
};

[[nodiscard]] Grouped32AdapterDispatchFixture
make_grouped32_two_record_adapter_fixture(bool late_failure)
{
    Grouped32AdapterDispatchFixture fixture{};
    const std::array<std::uint32_t, 2> packed{
        (9u << 8u) | (14u << 1u),
        (11u << 8u) | (15u << 1u),
    };
    const std::array<std::uint32_t, 2> tile_counts{1u, 1u};
    const std::array<std::uint32_t, 2> tile_offsets{0u, 1u};
    std::array<gv::GpuGrouped32SketchQueryPlanWire, 2> plans{};
    plans[0].first_tile = 0u;
    plans[0].tile_count = 1u;
    plans[0].query_length = 100u;
    plans[0].query_state_index = 0u;
    plans[0].anchor_base = 0u;
    plans[0].anchor_capacity = 4u;
    plans[0].batch_join_ordinal = (std::uint64_t{1} << 45u) + 7u;
    plans[1].first_tile = 1u;
    plans[1].tile_count = 1u;
    plans[1].query_length = 101u;
    plans[1].query_state_index = 1u;
    plans[1].anchor_base = 4u;
    plans[1].anchor_capacity = 4u;
    plans[1].batch_join_ordinal = (std::uint64_t{1} << 45u) + 9u;
    if (late_failure) plans[1].reserved1 = 1u;
    const std::array<gv::GpuGrouped32QueryRecordWire, 2> records{};
    const std::array<gv::GpuGrouped32QueryMinimizerWire, 2> minimizers{};
    gv::GpuGrouped32QueryAdapterStateWire adapter{};
    adapter.expected_records = 2u;
    adapter.completion = static_cast<std::uint32_t>(
        gv::GpuGrouped32ValidationCompletion::pending);
    const gv::GpuGrouped32ValidationStateWire validation{};
    const std::array<gv::GpuGrouped32DispatchIndirectCommandWire,
        static_cast<std::size_t>(gv::GpuGrouped32IndirectStage::count)>
        commands{};

    fixture.bindings.resize(9u);
    fixture.bindings[0] = pod_payload<std::uint32_t>(packed);
    fixture.bindings[1] = pod_payload<std::uint32_t>(tile_counts);
    fixture.bindings[2] = pod_payload<std::uint32_t>(tile_offsets);
    fixture.bindings[3] =
        pod_payload<gv::GpuGrouped32SketchQueryPlanWire>(plans);
    fixture.bindings[4] =
        pod_payload<gv::GpuGrouped32QueryRecordWire>(records);
    fixture.bindings[5] =
        pod_payload<gv::GpuGrouped32QueryMinimizerWire>(minimizers);
    fixture.bindings[6] = pod_payload(adapter);
    fixture.bindings[7] = pod_payload(validation);
    fixture.bindings[8] = pod_payload<
        gv::GpuGrouped32DispatchIndirectCommandWire>(commands);

    fixture.push.query_plan_count = 2u;
    fixture.push.tile_count = 2u;
    fixture.push.packed_minimizer_capacity = 2u;
    fixture.push.minimizer_word_kind = 1u;
    fixture.push.minimizer_words32 = 2u;
    fixture.push.hash_shift = 8u;
    fixture.push.pos_bits = 7u;
    fixture.push.kmer_size = 15u;
    fixture.push.adapter_state_index = 0u;
    fixture.push.adapter_state_count = 1u;
    fixture.push.validation_index = 0u;
    fixture.push.validation_state_count = 1u;
    fixture.push.indirect_base = 0u;
    fixture.push.indirect_command_count = static_cast<std::uint32_t>(
        gv::GpuGrouped32IndirectStage::count);
    fixture.push.max_workgroups_x = 2u;
    fixture.push.max_partition_groups_y = 1u;
    return fixture;
}

}  // namespace

TEST_CASE("DOC0341 shader variants enumerate the runtime AOT set",
          "[shader_cache][doc0341]") {
    gv::VulkanStaticVariantInputs inputs{
        .max_workgroup_invocations = 256,
        .max_shared_memory_bytes = 32u * 1024u,
        .default_window_size = 10,
    };

    std::vector<gv::ShaderVariantKey> keys;
    gv::for_each_static_shader_variant(inputs, [&](gv::ShaderVariantKey key) {
        INFO("variant stem: " << gv::cache_file_stem(key));
        CHECK(gv::valid_shader_variant_key(key));
        keys.push_back(key);
    });

    REQUIRE(keys.size() == 36);
    auto sorted = keys;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return gv::cache_file_stem(a) < gv::cache_file_stem(b);
    });
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    CHECK(gv::choose_chaining_variant(inputs) == gv::ChainingPipelineKey{32u, 32u});
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::chain_tiled_dp_u64, 32u, 32u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seed_lookup_anchors, 384u, 128u, 256u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_sketch_tile, 384u, 64u, 10u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_coordinate_vote_reduce, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_coordinate_vote_reduce_batch, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_persisted_directory_select, 384u, 128u, 1u)));
    CHECK_FALSE(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 128u, 2u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_banded_affine, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_simple_cigar, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_simple_cigar, 384u, 64u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_extract_key_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_tiebreak_generic, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_global_compare_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_dense_scatter_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_prefix_check_dna4, 384u, 256u, 0u)));

    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::chain_tiled_dp_u64, 384u, 256u, 0u)));
    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 128u)));
}

TEST_CASE("grouped32 ABI2 preseed variants are explicit and inactive",
          "[shader_cache][grouped32][abi2][aot]") {
    std::vector<gv::ShaderVariantKey> keys;
    gv::for_each_inactive_grouped32_shader_variant(
        [&](gv::ShaderVariantKey key) { keys.push_back(key); });
    REQUIRE(keys.size() == 11u);
    CHECK(is_hex_digest(gv::grouped32_inactive_shader_set_digest_hex()));

    gv::VulkanStaticVariantInputs inputs{
        .max_workgroup_invocations = 256u,
        .max_shared_memory_bytes = 32u * 1024u,
        .default_window_size = 10u,
    };
    std::vector<gv::ShaderVariantKey> active;
    gv::for_each_static_shader_variant(
        inputs, [&](gv::ShaderVariantKey key) { active.push_back(key); });
    for (const auto& key : keys) {
        CHECK(gv::valid_shader_variant_key(key));
        CHECK_FALSE(contains_key(active, key));
    }
}

TEST_CASE("format-4 grouped32 ABI2 pins every device wire",
          "[shader_cache][grouped32][abi2]") {
    STATIC_REQUIRE(gv::k_gpu_grouped32_abi_version == 2u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32ControlWire) == 12u);
    STATIC_REQUIRE(offsetof(gv::GpuGrouped32ControlWire, hash) == 0u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32ControlWire, position_offset) == 4u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32ControlWire, position_count) == 8u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32PositionWire) == 4u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32PartitionWire) == 64u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32PartitionWire, directory_ordinal) == 4u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32PartitionWire, segment_base_bp) == 40u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32PartitionWire, reserved1) == 60u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32QueryMinimizerWire) == 16u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32QueryRecordWire) == 48u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32OccurrenceStateWire) == 16u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32QueryStateWire) == 16u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32ValidationStateWire) == 32u);
    STATIC_REQUIRE(sizeof(
        gv::GpuGrouped32DispatchIndirectCommandWire) == 12u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32GlobalAnchorWire) == 24u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32AnchorRadixKeyWire) == 16u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32SortSegmentWire) == 24u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32FinalResultCigarWire) == 88u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32FinalResultCigarWire, batch_join_ordinal) == 64u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32Abi2Push) == 72u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32SketchQueryPlanWire) == 48u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32QueryAdapterStateWire) == 32u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32QueryAdapterPush) == 64u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32IndirectPreparePush) == 48u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32DeviceIdentity) == 32u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32ResidentIdentity) == 104u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32ExecutionIdentity) == 120u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32StageExecutionRecord) == 72u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32BarrierRecord) == 80u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32BarrierPlan) == 1216u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32StreamMatchWire) == 16u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32StreamStateWire) == 64u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32StreamPush) == 80u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        gv::GpuGrouped32PrefixAllocationStrategy::coarse_query_scan) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        gv::GpuGrouped32PrefixAllocationStrategy::hierarchical_segmented_scan)
        == 2u);
    STATIC_REQUIRE(std::is_standard_layout_v<
        gv::GpuGrouped32PartitionWire>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
        gv::GpuGrouped32ValidatedResidentProof>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
        gv::GpuGrouped32FoundationOnlyProof>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
        gv::GpuGrouped32ExecutionProof>);

    CHECK(gv::descriptor_binding_count(
        gv::DescriptorLayoutKind::phase_c_grouped32_abi2) == 13u);
    CHECK(gv::descriptor_binding_count(
        gv::DescriptorLayoutKind::phase_c_grouped32_adapter_abi2) == 9u);
    CHECK(gv::descriptor_binding_count(
        gv::DescriptorLayoutKind::phase_c_grouped32_stream) == 9u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_grouped32_accumulate_occurrences) == 72u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_grouped32_adapt_query_sketch) == 64u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_grouped32_prepare_indirect) == 48u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_grouped32_clear_state) == 72u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_grouped32_finalize_query_adapter) == 64u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_grouped32_finalize_validation) == 72u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_grouped32_stream_control) == 80u);
    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 128u, 1u)));
    CHECK(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 128u, 2u)));
}

TEST_CASE("format-4 bounded stream kernels compile and reflect exact raw ABI",
          "[shader_cache][grouped32][stream][reflection]") {
    const std::array kinds{
        gv::ShaderKind::phase_c_grouped32_stream_control,
        gv::ShaderKind::phase_c_grouped32_stream_authorize,
        gv::ShaderKind::phase_c_grouped32_stream_position,
        gv::ShaderKind::phase_c_grouped32_stream_seal,
    };
    for (const gv::ShaderKind kind : kinds) {
        const auto key = gv::make_shader_key(kind, 384u, 128u, 2u);
        CAPTURE(gv::cache_file_stem(key));
        REQUIRE(gv::valid_shader_variant_key(key));
        const auto spirv = gv::compile_shader_spirv(key.to_request());
        REQUIRE_FALSE(spirv.empty());
        REQUIRE(gv::validate_spirv_layout(key, spirv));
        const auto layout = gv::scan_spirv_layout(spirv);
        CHECK(layout.has_push_constant_offsets(
            std::array<std::uint32_t, 20>{
                0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 36u,
                40u, 44u, 48u, 52u, 56u, 60u, 64u, 68u, 72u, 76u}));
    }
}

TEST_CASE("format-4 grouped32 ABI2 inactive kernels compile and reflect",
          "[shader_cache][grouped32][abi2][reflection]") {
    const std::array kinds{
        gv::ShaderKind::phase_c_grouped32_validate,
        gv::ShaderKind::phase_c_grouped32_accumulate_occurrences,
        gv::ShaderKind::phase_c_grouped32_finalize_max_occ,
        gv::ShaderKind::phase_c_grouped32_allocate_anchor_prefix,
        gv::ShaderKind::phase_c_grouped32_emit_global_anchors,
        gv::ShaderKind::phase_c_grouped32_build_sort_segments,
        gv::ShaderKind::phase_c_grouped32_adapt_query_sketch,
        gv::ShaderKind::phase_c_grouped32_prepare_indirect,
        gv::ShaderKind::phase_c_grouped32_clear_state,
        gv::ShaderKind::phase_c_grouped32_finalize_query_adapter,
        gv::ShaderKind::phase_c_grouped32_finalize_validation,
    };
    for (const gv::ShaderKind kind : kinds) {
        const auto key = gv::make_shader_key(kind, 384u, 128u, 2u);
        CAPTURE(gv::cache_file_stem(key));
        REQUIRE(gv::valid_shader_variant_key(key));
        const auto spirv = gv::compile_shader_spirv(key.to_request());
        REQUIRE_FALSE(spirv.empty());
        REQUIRE(gv::validate_spirv_layout(key, spirv));
        const auto msl = gv::spirv_to_msl(spirv, key, gv::MslVersion{});
        REQUIRE(msl);
        CHECK(msl->wg_x == 128u);
        CHECK(msl->wg_y == 1u);
        CHECK(msl->wg_z == 1u);
        CHECK(msl->needs_buffer_size_buffer);
        CHECK(msl->buffer_size_buffer_index == 25u);
    }

    const auto validate_key = gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 128u, 2u);
    const auto validate_spirv =
        gv::compile_shader_spirv(validate_key.to_request());
    const auto layout = gv::scan_spirv_layout(validate_spirv);
    CHECK(layout.has_binding_array_stride(0u, 0u, 64u));
    CHECK(layout.has_binding_struct_offsets(
        0u, 0u, std::array<std::uint32_t, 14>{
            0u, 4u, 8u, 12u, 16u, 20u, 24u,
            28u, 32u, 36u, 40u, 48u, 56u, 60u}));
    CHECK_FALSE(layout.has_binding_array_stride(0u, 0u, 12u));
    CHECK(layout.has_binding_array_stride(0u, 1u, 12u));
    CHECK(layout.has_push_constant_offsets(
        std::array<std::uint32_t, 18>{
            0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u,
            36u, 40u, 44u, 48u, 52u, 56u, 60u, 64u, 68u}));
    const auto wrong_workgroup_key = gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 64u, 2u);
    CHECK_FALSE(gv::spirv_to_msl(
        validate_spirv, wrong_workgroup_key, gv::MslVersion{}));

}

TEST_CASE("grouped32 validation shaders reject malformed descriptors without writes",
          "[shader_cache][grouped32][abi2][vulkan][execution]") {
    // These checks are mandatory even on hosts without a usable Vulkan
    // device: every inactive ABI2 kernel must compile to structurally valid
    // SPIR-V and must retain runtime-array length instructions.
    constexpr std::uint16_t op_array_length = 68u;
    static const auto structural_modules = [] {
        const std::array kinds{
            gv::ShaderKind::phase_c_grouped32_clear_state,
            gv::ShaderKind::phase_c_grouped32_adapt_query_sketch,
            gv::ShaderKind::phase_c_grouped32_finalize_query_adapter,
            gv::ShaderKind::phase_c_grouped32_prepare_indirect,
            gv::ShaderKind::phase_c_grouped32_validate,
            gv::ShaderKind::phase_c_grouped32_finalize_validation,
            gv::ShaderKind::phase_c_grouped32_accumulate_occurrences,
            gv::ShaderKind::phase_c_grouped32_finalize_max_occ,
            gv::ShaderKind::phase_c_grouped32_allocate_anchor_prefix,
            gv::ShaderKind::phase_c_grouped32_build_sort_segments,
            gv::ShaderKind::phase_c_grouped32_emit_global_anchors,
        };
        std::vector<std::pair<gv::ShaderKind, std::vector<std::uint32_t>>>
            modules;
        modules.reserve(kinds.size());
        for (const auto kind : kinds) {
            const auto key = gv::make_shader_key(kind, 384u, 128u, 2u);
            modules.emplace_back(
                kind, gv::compile_shader_spirv(key.to_request()));
        }
        return modules;
    }();
    REQUIRE(structural_modules.size() == 11u);
    for (const auto& [kind, spirv] : structural_modules) {
        const auto key = gv::make_shader_key(kind, 384u, 128u, 2u);
        CAPTURE(gv::cache_file_stem(key));
        REQUIRE(gv::validate_spirv_layout(key, spirv));
        REQUIRE(spirv_opcode_count(spirv, op_array_length) != 0u);
    }

    const auto cpu_fixture = make_grouped32_core_fixture(true, 13u);
    const auto cpu_partition = pod_from_payload<
        gv::GpuGrouped32PartitionWire>(cpu_fixture.bindings[0]);
    const auto cpu_control = pod_from_payload<
        gv::GpuGrouped32ControlWire>(cpu_fixture.bindings[1]);
    const std::array cpu_positions{
        pod_from_payload<gv::GpuGrouped32PositionWire>(
            cpu_fixture.bindings[2], 0u),
        pod_from_payload<gv::GpuGrouped32PositionWire>(
            cpu_fixture.bindings[2], sizeof(gv::GpuGrouped32PositionWire)),
    };
    REQUIRE((grouped32_partition_validation_oracle(
               cpu_partition, std::span{&cpu_control, 1u}, cpu_positions)
        & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::position_order)) != 0u);
    REQUIRE_FALSE(grouped32_query_encoding_oracle(
        pod_from_payload<gv::GpuGrouped32QueryRecordWire>(
            cpu_fixture.bindings[3]),
        pod_from_payload<gv::GpuGrouped32QueryMinimizerWire>(
            cpu_fixture.bindings[4])));
    const std::uint32_t cpu_global_count =
        grouped32_saturating_count_oracle(
            grouped32_saturating_count_oracle(0u, 3u, 5u), 4u, 5u);
    REQUIRE(cpu_global_count == 6u);
    REQUIRE(grouped32_finalize_count_oracle(cpu_global_count, 5u) == 0u);
    gv::GpuGrouped32ValidationStateWire cpu_validation{};
    gv::GpuGrouped32QueryAdapterStateWire cpu_adapter{};
    REQUIRE_FALSE(grouped32_gate_oracle(cpu_validation, cpu_adapter));
    cpu_validation.completion = static_cast<std::uint32_t>(
        gv::GpuGrouped32ValidationCompletion::accepted);
    cpu_adapter.completion = static_cast<std::uint32_t>(
        gv::GpuGrouped32ValidationCompletion::accepted);
    REQUIRE(grouped32_gate_oracle(cpu_validation, cpu_adapter));

    std::unique_ptr<Grouped32ValidationHarness> harness;
    try {
        harness = std::make_unique<Grouped32ValidationHarness>();
    } catch (const std::exception& error) {
        // This suite uses Catch2; SKIP is its explicit GTEST_SKIP equivalent.
        // A skipped result is never acceptable as local GPU execution proof.
        SKIP("grouped32 Vulkan execution unavailable: " << error.what()
            << "; final acceptance requires a non-skipped local "
               "MoltenVK or hardware Vulkan run");
    }
    REQUIRE(harness != nullptr);

    {
        INFO("every pushed runtime-array count rejects beyond its descriptor");
        const auto base = make_grouped32_core_fixture().push;
        std::vector<gv::GpuGrouped32Abi2Push> malformed;
        malformed.reserve(10u);
        auto add = [&](auto mutate) {
            auto push = base;
            mutate(push);
            malformed.push_back(push);
        };
        add([](auto& push) { push.partition_count = 2u; });
        add([](auto& push) { push.control_count = 2u; });
        add([](auto& push) { push.position_count = 3u; });
        add([](auto& push) { push.query_record_count = 2u; });
        add([](auto& push) { push.query_minimizer_capacity = 2u; });
        add([](auto& push) { push.occurrence_count = 2u; });
        add([](auto& push) { push.anchor_capacity = 5u; });
        add([](auto& push) { push.validation_state_count = 2u; });
        add([](auto& push) { push.indirect_command_count = 8u; });
        add([](auto& push) { push.adapter_state_count = 2u; });
        REQUIRE(malformed.size() == 10u);
        std::size_t malformed_executed = 0u;
        for (std::size_t case_index = 0u; case_index < malformed.size();
             ++case_index) {
            CAPTURE(case_index);
            auto fixture = make_grouped32_core_fixture();
            fixture.push = malformed[case_index];
            const auto push = pod_payload(fixture.push);
            auto validate_result = harness->run(
                gv::ShaderKind::phase_c_grouped32_validate, push,
                std::move(fixture.bindings));
            REQUIRE(validate_result.guards_intact);
            const auto pending = pod_from_payload<
                gv::GpuGrouped32ValidationStateWire>(
                    validate_result.bindings[8]);
            CHECK(pending.completion == static_cast<std::uint32_t>(
                gv::GpuGrouped32ValidationCompletion::pending));
            const auto result = harness->run(
                gv::ShaderKind::phase_c_grouped32_finalize_validation,
                push, std::move(validate_result.bindings));
            REQUIRE(result.guards_intact);
            const auto validation = pod_from_payload<
                gv::GpuGrouped32ValidationStateWire>(result.bindings[8]);
            CHECK((validation.status & static_cast<std::uint32_t>(
                gv::GpuGrouped32ValidationError::descriptor_bounds)) != 0u);
            CHECK(validation.completion == static_cast<std::uint32_t>(
                gv::GpuGrouped32ValidationCompletion::rejected));
            ++malformed_executed;
        }
        CHECK(malformed_executed == malformed.size());
    }

    {
        INFO("duplicate position and q_last before K reject together");
        auto fixture = make_grouped32_core_fixture(true, 13u);
        const auto push = pod_payload(fixture.push);
        auto validate_result = harness->run(
            gv::ShaderKind::phase_c_grouped32_validate, push,
            std::move(fixture.bindings));
        REQUIRE(validate_result.guards_intact);
        CHECK(pod_from_payload<gv::GpuGrouped32ValidationStateWire>(
                  validate_result.bindings[8]).completion
            == static_cast<std::uint32_t>(
                gv::GpuGrouped32ValidationCompletion::pending));
        const auto result = harness->run(
            gv::ShaderKind::phase_c_grouped32_finalize_validation,
            push, std::move(validate_result.bindings));
        REQUIRE(result.guards_intact);
        const auto validation = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(result.bindings[8]);
        CHECK((validation.status & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::position_order)) != 0u);
        CHECK((validation.status & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::query_bounds)) != 0u);
        CHECK(validation.completion == static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::rejected));
    }

    {
        INFO("short query state, key, segment, and command descriptors reject");
        constexpr std::array<std::size_t, 5> shortened_bindings{
            3u, 6u, 9u, 10u, 11u};
        std::size_t shortened_executed = 0u;
        for (const std::size_t binding : shortened_bindings) {
            CAPTURE(binding);
            auto fixture = make_grouped32_core_fixture();
            fixture.bindings[binding].assign(4u, std::byte{0});
            const auto push = pod_payload(fixture.push);
            auto validate_result = harness->run(
                gv::ShaderKind::phase_c_grouped32_validate, push,
                std::move(fixture.bindings));
            REQUIRE(validate_result.guards_intact);
            const auto result = harness->run(
                gv::ShaderKind::phase_c_grouped32_finalize_validation,
                push, std::move(validate_result.bindings));
            REQUIRE(result.guards_intact);
            const auto validation = pod_from_payload<
                gv::GpuGrouped32ValidationStateWire>(result.bindings[8]);
            CHECK((validation.status & static_cast<std::uint32_t>(
                gv::GpuGrouped32ValidationError::descriptor_bounds)) != 0u);
            CHECK(validation.completion == static_cast<std::uint32_t>(
                gv::GpuGrouped32ValidationCompletion::rejected));
            ++shortened_executed;
        }
        CHECK(shortened_executed == shortened_bindings.size());
    }

    {
        INFO("valid global completion opens commands but leaves radix disabled");
        auto fixture = make_grouped32_core_fixture(false, 14u);
        const auto push = pod_payload(fixture.push);
        gv::GpuGrouped32IndirectPreparePush prepare_push{};
        prepare_push.partition_count = fixture.push.partition_count;
        prepare_push.query_record_count = fixture.push.query_record_count;
        prepare_push.adapter_state_index = fixture.push.adapter_state_index;
        prepare_push.adapter_state_count = fixture.push.adapter_state_count;
        prepare_push.validation_index = fixture.push.validation_index;
        prepare_push.validation_state_count =
            fixture.push.validation_state_count;
        prepare_push.indirect_base = fixture.push.indirect_base;
        prepare_push.indirect_command_count =
            fixture.push.indirect_command_count;
        prepare_push.max_workgroups_x = fixture.push.max_workgroups_x;
        prepare_push.max_partition_groups_y =
            fixture.push.max_partition_groups_y;
        prepare_push.local_size_x = 128u;
        auto prepare_result = harness->run(
            gv::ShaderKind::phase_c_grouped32_prepare_indirect,
            pod_payload(prepare_push), std::move(fixture.bindings));
        REQUIRE(prepare_result.guards_intact);
        const auto prepared_validation = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(
                prepare_result.bindings[8]);
        CHECK(prepared_validation.status == 0u);
        CHECK(prepared_validation.expected_workgroups == 1u);
        CHECK(prepared_validation.completion == static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::pending));
        const auto prepared_validate_command = pod_from_payload<
            gv::GpuGrouped32DispatchIndirectCommandWire>(
                prepare_result.bindings[9], 0u);
        CHECK(prepared_validate_command.group_count_x == 1u);
        CHECK(prepared_validate_command.group_count_y == 1u);
        CHECK(prepared_validate_command.group_count_z == 1u);
        auto validate_result = harness->run(
            gv::ShaderKind::phase_c_grouped32_validate, push,
            std::move(prepare_result.bindings));
        REQUIRE(validate_result.guards_intact);
        const auto pending = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(
                validate_result.bindings[8]);
        CHECK(pending.status == 0u);
        CHECK(pending.completed_workgroups == 1u);
        CHECK(pending.completion == static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::pending));
        for (std::size_t stage = 0u;
             stage < static_cast<std::size_t>(
                 gv::GpuGrouped32IndirectStage::count); ++stage) {
            const auto command = pod_from_payload<
                gv::GpuGrouped32DispatchIndirectCommandWire>(
                    validate_result.bindings[9], stage * sizeof(
                        gv::GpuGrouped32DispatchIndirectCommandWire));
            const bool validate_stage = stage == static_cast<std::size_t>(
                gv::GpuGrouped32IndirectStage::validate);
            CHECK(command.group_count_x == (validate_stage ? 1u : 0u));
            CHECK(command.group_count_y == (validate_stage ? 1u : 0u));
            CHECK(command.group_count_z == (validate_stage ? 1u : 0u));
        }
        const auto result = harness->run(
            gv::ShaderKind::phase_c_grouped32_finalize_validation,
            push, std::move(validate_result.bindings));
        REQUIRE(result.guards_intact);
        const auto validation = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(result.bindings[8]);
        CHECK(validation.status == 0u);
        CHECK(validation.completion == static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::accepted));
        const std::size_t accumulate_offset = sizeof(
            gv::GpuGrouped32DispatchIndirectCommandWire)
            * static_cast<std::size_t>(
                gv::GpuGrouped32IndirectStage::accumulate_occurrences);
        const auto accumulate = pod_from_payload<
            gv::GpuGrouped32DispatchIndirectCommandWire>(
                result.bindings[9], accumulate_offset);
        CHECK(accumulate.group_count_x == 1u);
        CHECK(accumulate.group_count_y == 1u);
        const std::size_t radix_offset = sizeof(
            gv::GpuGrouped32DispatchIndirectCommandWire)
            * static_cast<std::size_t>(
                gv::GpuGrouped32IndirectStage::radix_sort);
        const auto radix = pod_from_payload<
            gv::GpuGrouped32DispatchIndirectCommandWire>(
                result.bindings[9], radix_offset);
        CHECK(radix.group_count_x == 0u);
        CHECK(radix.group_count_y == 0u);
        CHECK(radix.group_count_z == 0u);
    }

    {
        INFO("rejected completion gates downstream state writes");
        auto fixture = make_grouped32_core_fixture();
        auto validation = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(fixture.bindings[8]);
        validation.status = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::query_bounds);
        validation.completion = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::rejected);
        fixture.bindings[8] = pod_payload(validation);
        gv::GpuGrouped32OccurrenceStateWire occurrence{};
        occurrence.global_count = 7u;
        occurrence.accepted_count = 6u;
        occurrence.anchor_offset = 5u;
        occurrence.emitted_count = 4u;
        fixture.bindings[5] = pod_payload(occurrence);
        gv::GpuGrouped32QueryStateWire state{};
        state.anchor_count = 3u;
        state.rejected_minimizers = 2u;
        state.completed_minimizers = 1u;
        fixture.bindings[6] = pod_payload(state);
        const auto occurrence_before = fixture.bindings[5];
        const auto state_before = fixture.bindings[6];
        const auto push = pod_payload(fixture.push);
        const auto result = harness->run(
            gv::ShaderKind::phase_c_grouped32_finalize_max_occ, push,
            std::move(fixture.bindings));
        REQUIRE(result.guards_intact);
        CHECK(result.bindings[5] == occurrence_before);
        CHECK(result.bindings[6] == state_before);
    }

    {
        INFO("indirect preparation bounds partition count against binding zero");
        auto fixture = make_grouped32_core_fixture();
        gv::GpuGrouped32IndirectPreparePush prepare{};
        prepare.partition_count = 2u;
        prepare.query_record_count = 1u;
        prepare.adapter_state_index = 0u;
        prepare.adapter_state_count = 1u;
        prepare.validation_index = 0u;
        prepare.validation_state_count = 1u;
        prepare.indirect_base = 0u;
        prepare.indirect_command_count = static_cast<std::uint32_t>(
            gv::GpuGrouped32IndirectStage::count);
        prepare.max_workgroups_x = 1u;
        prepare.max_partition_groups_y = 1u;
        prepare.local_size_x = 128u;
        const auto push = pod_payload(prepare);
        const auto result = harness->run(
            gv::ShaderKind::phase_c_grouped32_prepare_indirect, push,
            std::move(fixture.bindings));
        REQUIRE(result.guards_intact);
        const auto rejected = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(result.bindings[8]);
        CHECK((rejected.status & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::descriptor_bounds)) != 0u);
        CHECK(rejected.completion == static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::rejected));
    }

    {
        INFO("sketch adapter rejects a short query-plan descriptor");
        std::vector<std::vector<std::byte>> bindings(9u);
        const std::array<std::uint32_t, 1> packed{
            (9u << 8u) | (14u << 1u)};
        const std::array<std::uint32_t, 1> tile_counts{1u};
        const std::array<std::uint32_t, 1> tile_offsets{0u};
        const gv::GpuGrouped32SketchQueryPlanWire plan{
            .first_tile = 0u,
            .tile_count = 1u,
            .query_length = 100u,
            .query_state_index = 0u,
            .anchor_base = 0u,
            .anchor_capacity = 4u,
            .batch_join_ordinal = 7u,
        };
        const gv::GpuGrouped32QueryRecordWire record{};
        const gv::GpuGrouped32QueryMinimizerWire minimizer{};
        gv::GpuGrouped32QueryAdapterStateWire adapter{};
        adapter.expected_records = 2u;
        adapter.completion = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::pending);
        const gv::GpuGrouped32ValidationStateWire validation{};
        const std::array<gv::GpuGrouped32DispatchIndirectCommandWire,
            static_cast<std::size_t>(gv::GpuGrouped32IndirectStage::count)>
            commands{};
        bindings[0] = pod_payload<std::uint32_t>(packed);
        bindings[1] = pod_payload<std::uint32_t>(tile_counts);
        bindings[2] = pod_payload<std::uint32_t>(tile_offsets);
        bindings[3] = pod_payload(plan);
        bindings[4] = pod_payload(record);
        bindings[5] = pod_payload(minimizer);
        bindings[6] = pod_payload(adapter);
        bindings[7] = pod_payload(validation);
        bindings[8] = pod_payload<
            gv::GpuGrouped32DispatchIndirectCommandWire>(commands);
        gv::GpuGrouped32QueryAdapterPush adapter_push{};
        adapter_push.query_plan_count = 2u;
        adapter_push.tile_count = 1u;
        adapter_push.packed_minimizer_capacity = 1u;
        adapter_push.minimizer_word_kind = 1u;
        adapter_push.minimizer_words32 = 1u;
        adapter_push.hash_shift = 8u;
        adapter_push.pos_bits = 7u;
        adapter_push.kmer_size = 15u;
        adapter_push.adapter_state_index = 0u;
        adapter_push.adapter_state_count = 1u;
        adapter_push.validation_index = 0u;
        adapter_push.validation_state_count = 1u;
        adapter_push.indirect_base = 0u;
        adapter_push.indirect_command_count = static_cast<std::uint32_t>(
            gv::GpuGrouped32IndirectStage::count);
        adapter_push.max_workgroups_x = 2u;
        adapter_push.max_partition_groups_y = 1u;
        const auto push = pod_payload(adapter_push);
        auto adapter_result = harness->run(
            gv::ShaderKind::phase_c_grouped32_adapt_query_sketch, push,
            std::move(bindings), 2u);
        REQUIRE(adapter_result.guards_intact);
        CHECK(pod_from_payload<gv::GpuGrouped32QueryAdapterStateWire>(
                  adapter_result.bindings[6]).completion
            == static_cast<std::uint32_t>(
                gv::GpuGrouped32ValidationCompletion::pending));
        const auto result = harness->run(
            gv::ShaderKind::phase_c_grouped32_finalize_query_adapter,
            push, std::move(adapter_result.bindings));
        REQUIRE(result.guards_intact);
        const auto rejected = pod_from_payload<
            gv::GpuGrouped32QueryAdapterStateWire>(result.bindings[6]);
        CHECK((rejected.status & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::descriptor_bounds)) != 0u);
        CHECK(rejected.completion == static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::rejected));
    }

    {
        INFO("operation clear removes every dirty state field and command");
        auto fixture = make_grouped32_core_fixture();
        const gv::GpuGrouped32OccurrenceStateWire dirty_occurrence{
            .global_count = 1u,
            .accepted_count = 2u,
            .anchor_offset = 3u,
            .emitted_count = 4u,
        };
        const gv::GpuGrouped32QueryStateWire dirty_query{
            .anchor_count = 5u,
            .rejected_minimizers = 6u,
            .completed_minimizers = 7u,
            .status = 8u,
        };
        const gv::GpuGrouped32SortSegmentWire dirty_segment{
            .anchor_begin = 9u,
            .anchor_count = 10u,
            .query_state_index = 11u,
            .flags = 12u,
            .batch_join_ordinal = 13u,
        };
        gv::GpuGrouped32ValidationStateWire dirty_validation{};
        dirty_validation.status = 14u;
        dirty_validation.failed_partitions = 15u;
        dirty_validation.controls_checked = 16u;
        dirty_validation.positions_checked = 17u;
        dirty_validation.completed_workgroups = 18u;
        dirty_validation.expected_workgroups = 19u;
        dirty_validation.completion = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::accepted);
        dirty_validation.reserved = 20u;
        gv::GpuGrouped32QueryAdapterStateWire dirty_adapter{};
        dirty_adapter.status = 21u;
        dirty_adapter.completed_records = 22u;
        dirty_adapter.expected_records = 23u;
        dirty_adapter.total_minimizers = 24u;
        dirty_adapter.max_query_minimizers = 25u;
        dirty_adapter.completion = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::accepted);
        dirty_adapter.reserved0 = 26u;
        dirty_adapter.reserved1 = 27u;
        std::array<gv::GpuGrouped32DispatchIndirectCommandWire,
            static_cast<std::size_t>(gv::GpuGrouped32IndirectStage::count)
                + 2u>
            dirty_commands{};
        for (auto& command : dirty_commands) {
            command = {.group_count_x = 28u,
                       .group_count_y = 29u,
                       .group_count_z = 30u};
        }
        fixture.bindings[5] = pod_payload(dirty_occurrence);
        fixture.bindings[6] = pod_payload(dirty_query);
        fixture.bindings[8] = pod_payload(dirty_validation);
        fixture.bindings[9] = pod_payload<
            gv::GpuGrouped32DispatchIndirectCommandWire>(dirty_commands);
        fixture.bindings[11] = pod_payload(dirty_segment);
        fixture.bindings[12] = pod_payload(dirty_adapter);
        fixture.push.indirect_base = 1u;
        fixture.push.indirect_command_count =
            static_cast<std::uint32_t>(dirty_commands.size());
        const auto push = pod_payload(fixture.push);
        const auto result = harness->run(
            gv::ShaderKind::phase_c_grouped32_clear_state, push,
            std::move(fixture.bindings), 2u);
        REQUIRE(result.guards_intact);
        const auto occurrence = pod_from_payload<
            gv::GpuGrouped32OccurrenceStateWire>(result.bindings[5]);
        CHECK(occurrence.global_count == 0u);
        CHECK(occurrence.accepted_count == 0u);
        CHECK(occurrence.anchor_offset == 0u);
        CHECK(occurrence.emitted_count == 0u);
        const auto query = pod_from_payload<gv::GpuGrouped32QueryStateWire>(
            result.bindings[6]);
        CHECK(query.anchor_count == 0u);
        CHECK(query.rejected_minimizers == 0u);
        CHECK(query.completed_minimizers == 0u);
        CHECK(query.status == 0u);
        const auto validation = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(result.bindings[8]);
        CHECK(validation.status == 0u);
        CHECK(validation.failed_partitions == 0u);
        CHECK(validation.controls_checked == 0u);
        CHECK(validation.positions_checked == 0u);
        CHECK(validation.completed_workgroups == 0u);
        CHECK(validation.expected_workgroups == 0u);
        CHECK(validation.completion == static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::pending));
        CHECK(validation.reserved == 0u);
        const auto adapter = pod_from_payload<
            gv::GpuGrouped32QueryAdapterStateWire>(result.bindings[12]);
        CHECK(adapter.status == 0u);
        CHECK(adapter.completed_records == 0u);
        CHECK(adapter.expected_records == fixture.push.query_record_count);
        CHECK(adapter.total_minimizers == 0u);
        CHECK(adapter.max_query_minimizers == 0u);
        CHECK(adapter.completion == static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::pending));
        CHECK(adapter.reserved0 == 0u);
        CHECK(adapter.reserved1 == 0u);
        const auto segment = pod_from_payload<
            gv::GpuGrouped32SortSegmentWire>(result.bindings[11]);
        CHECK(segment.anchor_begin == 0u);
        CHECK(segment.anchor_count == 0u);
        CHECK(segment.query_state_index == 0u);
        CHECK(segment.flags == 0u);
        CHECK(segment.batch_join_ordinal == 0u);
        for (std::size_t stage = 0u; stage < dirty_commands.size(); ++stage) {
            const auto command = pod_from_payload<
                gv::GpuGrouped32DispatchIndirectCommandWire>(
                    result.bindings[9], stage * sizeof(
                        gv::GpuGrouped32DispatchIndirectCommandWire));
            const bool operation_command = stage >= 1u
                && stage < 1u + static_cast<std::size_t>(
                    gv::GpuGrouped32IndirectStage::count);
            CHECK(command.group_count_x
                == (operation_command ? 0u : 28u));
            CHECK(command.group_count_y
                == (operation_command ? 0u : 29u));
            CHECK(command.group_count_z
                == (operation_command ? 0u : 30u));
        }
    }

    {
        INFO("two validation workgroups require a separate late-safe finalizer");
        std::size_t validation_cases_executed = 0u;
        for (const bool late_failure : {false, true}) {
            CAPTURE(late_failure);
            auto fixture =
                make_grouped32_multiworkgroup_validation_fixture(
                    late_failure);
            const auto push = pod_payload(fixture.push);
            auto validate_result = harness->run(
                gv::ShaderKind::phase_c_grouped32_validate, push,
                std::move(fixture.bindings), 2u);
            REQUIRE(validate_result.guards_intact);
            const auto pending = pod_from_payload<
                gv::GpuGrouped32ValidationStateWire>(
                    validate_result.bindings[8]);
            CHECK(pending.completed_workgroups == 2u);
            CHECK(pending.completion == static_cast<std::uint32_t>(
                gv::GpuGrouped32ValidationCompletion::pending));
            if (late_failure) {
                CHECK((pending.status & static_cast<std::uint32_t>(
                    gv::GpuGrouped32ValidationError::query_bounds)) != 0u);
            } else {
                CHECK(pending.status == 0u);
            }
            const std::size_t segment_offset = sizeof(
                gv::GpuGrouped32DispatchIndirectCommandWire)
                * static_cast<std::size_t>(
                    gv::GpuGrouped32IndirectStage::build_sort_segments);
            const auto pending_segment_command = pod_from_payload<
                gv::GpuGrouped32DispatchIndirectCommandWire>(
                    validate_result.bindings[9], segment_offset);
            CHECK(pending_segment_command.group_count_x == 77u);
            CHECK(pending_segment_command.group_count_y == 78u);
            CHECK(pending_segment_command.group_count_z == 79u);
            const auto result = harness->run(
                gv::ShaderKind::phase_c_grouped32_finalize_validation,
                push, std::move(validate_result.bindings));
            REQUIRE(result.guards_intact);
            const auto finalized = pod_from_payload<
                gv::GpuGrouped32ValidationStateWire>(result.bindings[8]);
            CHECK(finalized.completed_workgroups == 2u);
            CHECK(finalized.completion == static_cast<std::uint32_t>(
                late_failure
                    ? gv::GpuGrouped32ValidationCompletion::rejected
                    : gv::GpuGrouped32ValidationCompletion::accepted));
            const auto segment_command = pod_from_payload<
                gv::GpuGrouped32DispatchIndirectCommandWire>(
                    result.bindings[9], segment_offset);
            CHECK(segment_command.group_count_x
                == (late_failure ? 0u : 2u));
            const std::size_t radix_offset = sizeof(
                gv::GpuGrouped32DispatchIndirectCommandWire)
                * static_cast<std::size_t>(
                    gv::GpuGrouped32IndirectStage::radix_sort);
            const auto radix = pod_from_payload<
                gv::GpuGrouped32DispatchIndirectCommandWire>(
                    result.bindings[9], radix_offset);
            CHECK(radix.group_count_x == 0u);
            CHECK(radix.group_count_y == 0u);
            CHECK(radix.group_count_z == 0u);
            ++validation_cases_executed;
        }
        CHECK(validation_cases_executed == 2u);
    }

    {
        INFO("two-workgroup adapter preserves decoded values and u64 identity");
        std::size_t adapter_cases_executed = 0u;
        for (const bool late_failure : {false, true}) {
            CAPTURE(late_failure);
            auto fixture = make_grouped32_two_record_adapter_fixture(
                late_failure);
            const auto push = pod_payload(fixture.push);
            auto adapter_result = harness->run(
                gv::ShaderKind::phase_c_grouped32_adapt_query_sketch,
                push, std::move(fixture.bindings), 2u);
            REQUIRE(adapter_result.guards_intact);
            const auto pending = pod_from_payload<
                gv::GpuGrouped32QueryAdapterStateWire>(
                    adapter_result.bindings[6]);
            CHECK(pending.completed_records == 2u);
            CHECK(pending.completion == static_cast<std::uint32_t>(
                gv::GpuGrouped32ValidationCompletion::pending));
            if (!late_failure) {
                CHECK(pending.status == 0u);
                CHECK(pending.total_minimizers == 2u);
                CHECK(pending.max_query_minimizers == 1u);
                const auto record0 = pod_from_payload<
                    gv::GpuGrouped32QueryRecordWire>(
                        adapter_result.bindings[4], 0u);
                const auto record1 = pod_from_payload<
                    gv::GpuGrouped32QueryRecordWire>(
                        adapter_result.bindings[4], sizeof(record0));
                CHECK(record0.batch_join_ordinal
                    == (std::uint64_t{1} << 45u) + 7u);
                CHECK(record1.batch_join_ordinal
                    == (std::uint64_t{1} << 45u) + 9u);
                const auto minimizer0 = pod_from_payload<
                    gv::GpuGrouped32QueryMinimizerWire>(
                        adapter_result.bindings[5], 0u);
                const auto minimizer1 = pod_from_payload<
                    gv::GpuGrouped32QueryMinimizerWire>(
                        adapter_result.bindings[5], sizeof(minimizer0));
                CHECK(minimizer0.hash == 9u);
                CHECK(minimizer0.query_bp_strand == (14u << 1u));
                CHECK(minimizer0.occurrence_index == 0u);
                CHECK(minimizer0.query_record_index == 0u);
                CHECK(minimizer1.hash == 11u);
                CHECK(minimizer1.query_bp_strand == (15u << 1u));
                CHECK(minimizer1.occurrence_index == 1u);
                CHECK(minimizer1.query_record_index == 1u);
            }
            const auto result = harness->run(
                gv::ShaderKind::phase_c_grouped32_finalize_query_adapter,
                push, std::move(adapter_result.bindings));
            REQUIRE(result.guards_intact);
            const auto finalized = pod_from_payload<
                gv::GpuGrouped32QueryAdapterStateWire>(result.bindings[6]);
            CHECK(finalized.completion == static_cast<std::uint32_t>(
                late_failure
                    ? gv::GpuGrouped32ValidationCompletion::rejected
                    : gv::GpuGrouped32ValidationCompletion::accepted));
            if (late_failure) {
                CHECK((finalized.status & static_cast<std::uint32_t>(
                    gv::GpuGrouped32ValidationError::query_bounds)) != 0u);
            } else {
                CHECK(finalized.status == 0u);
            }
            ++adapter_cases_executed;
        }
        CHECK(adapter_cases_executed == 2u);
    }

    {
        INFO("target-global max-occ combines two partitions before filtering");
        auto fixture = make_grouped32_core_fixture();
        const std::array partitions{
            gv::GpuGrouped32PartitionWire{
                .partition_id = 3u,
                .directory_ordinal = 0u,
                .position_segment = 0u,
                .control_offset = 0u,
                .control_count = 1u,
                .position_offset = 0u,
                .position_count = 3u,
                .low_hash = 9u,
                .high_hash = 9u,
                .segment_base_bp = 0u,
                .segment_end_bp = 100u,
            },
            gv::GpuGrouped32PartitionWire{
                .partition_id = 4u,
                .directory_ordinal = 1u,
                .position_segment = 1u,
                .control_offset = 1u,
                .control_count = 1u,
                .position_offset = 3u,
                .position_count = 4u,
                .low_hash = 9u,
                .high_hash = 9u,
                .segment_base_bp = 100u,
                .segment_end_bp = 200u,
            },
        };
        const std::array controls{
            gv::GpuGrouped32ControlWire{
                .hash = 9u, .position_offset = 0u, .position_count = 3u},
            gv::GpuGrouped32ControlWire{
                .hash = 9u, .position_offset = 0u, .position_count = 4u},
        };
        const std::array positions{
            gv::GpuGrouped32PositionWire{.local_bp_strand = 2u},
            gv::GpuGrouped32PositionWire{.local_bp_strand = 4u},
            gv::GpuGrouped32PositionWire{.local_bp_strand = 6u},
            gv::GpuGrouped32PositionWire{.local_bp_strand = 8u},
            gv::GpuGrouped32PositionWire{.local_bp_strand = 10u},
            gv::GpuGrouped32PositionWire{.local_bp_strand = 12u},
            gv::GpuGrouped32PositionWire{.local_bp_strand = 14u},
        };
        auto record = pod_from_payload<gv::GpuGrouped32QueryRecordWire>(
            fixture.bindings[3]);
        record.anchor_capacity = 10u;
        gv::GpuGrouped32ValidationStateWire validation{};
        validation.completed_workgroups = 1u;
        validation.expected_workgroups = 1u;
        validation.completion = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::accepted);
        const std::array<gv::GpuGrouped32GlobalAnchorWire, 10> anchors{};
        const std::array<gv::GpuGrouped32AnchorRadixKeyWire, 10> keys{};
        fixture.bindings[0] =
            pod_payload<gv::GpuGrouped32PartitionWire>(partitions);
        fixture.bindings[1] =
            pod_payload<gv::GpuGrouped32ControlWire>(controls);
        fixture.bindings[2] =
            pod_payload<gv::GpuGrouped32PositionWire>(positions);
        fixture.bindings[3] = pod_payload(record);
        fixture.bindings[7] =
            pod_payload<gv::GpuGrouped32GlobalAnchorWire>(anchors);
        fixture.bindings[8] = pod_payload(validation);
        fixture.bindings[10] =
            pod_payload<gv::GpuGrouped32AnchorRadixKeyWire>(keys);
        fixture.push.partition_count = 2u;
        fixture.push.control_count = 2u;
        fixture.push.position_count = 7u;
        fixture.push.anchor_capacity = 10u;
        fixture.push.max_occ = 5u;
        fixture.push.max_partition_groups_y = 2u;
        const auto push = pod_payload(fixture.push);
        auto accumulate = harness->run(
            gv::ShaderKind::phase_c_grouped32_accumulate_occurrences,
            push, std::move(fixture.bindings));
        REQUIRE(accumulate.guards_intact);
        CHECK(pod_from_payload<gv::GpuGrouped32OccurrenceStateWire>(
                  accumulate.bindings[5]).global_count == 6u);
        const auto result = harness->run(
            gv::ShaderKind::phase_c_grouped32_finalize_max_occ,
            push, std::move(accumulate.bindings));
        REQUIRE(result.guards_intact);
        const auto occurrence = pod_from_payload<
            gv::GpuGrouped32OccurrenceStateWire>(result.bindings[5]);
        CHECK(occurrence.global_count == 6u);
        CHECK(occurrence.accepted_count == 0u);
        const auto query = pod_from_payload<gv::GpuGrouped32QueryStateWire>(
            result.bindings[6]);
        CHECK(query.rejected_minimizers == 1u);
        CHECK(query.completed_minimizers == 1u);
    }

    {
        INFO("global anchor emission executes a non-empty validated slice");
        auto fixture = make_grouped32_core_fixture();
        auto validation = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(fixture.bindings[8]);
        validation.completion = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::accepted);
        fixture.bindings[8] = pod_payload(validation);
        gv::GpuGrouped32OccurrenceStateWire occurrence{};
        occurrence.global_count = 2u;
        occurrence.accepted_count = 2u;
        occurrence.anchor_offset = 0u;
        fixture.bindings[5] = pod_payload(occurrence);
        const auto push = pod_payload(fixture.push);
        const auto result = harness->run(
            gv::ShaderKind::phase_c_grouped32_emit_global_anchors,
            push, std::move(fixture.bindings));
        REQUIRE(result.guards_intact);
        const auto emitted = pod_from_payload<
            gv::GpuGrouped32OccurrenceStateWire>(result.bindings[5]);
        CHECK(emitted.emitted_count == 2u);
        const auto anchor0 = pod_from_payload<
            gv::GpuGrouped32GlobalAnchorWire>(result.bindings[7], 0u);
        const auto anchor1 = pod_from_payload<
            gv::GpuGrouped32GlobalAnchorWire>(
                result.bindings[7], sizeof(anchor0));
        CHECK(anchor0.ref_position == 4u);
        CHECK(anchor1.ref_position == 5u);
        CHECK(anchor0.query_position == 14u);
        CHECK(anchor1.query_position == 14u);
        CHECK(anchor0.is_reverse == 0u);
        CHECK(anchor1.is_reverse == 0u);
        CHECK(anchor0.query_span == 15u);
        CHECK(anchor1.query_span == 15u);
        const auto key0 = pod_from_payload<
            gv::GpuGrouped32AnchorRadixKeyWire>(result.bindings[10], 0u);
        const auto key1 = pod_from_payload<
            gv::GpuGrouped32AnchorRadixKeyWire>(
                result.bindings[10], sizeof(key0));
        CHECK(key0.ref_position == 4u);
        CHECK(key1.ref_position == 5u);
        CHECK(key0.query_state_index == 0u);
        CHECK(key1.query_state_index == 0u);
        CHECK(pod_from_payload<gv::GpuGrouped32ValidationStateWire>(
                  result.bindings[8]).status == 0u);
    }

    {
        INFO("prefix and segment dispatches preserve per-query ownership");
        auto fixture = make_grouped32_core_fixture();
        const std::array records{
            gv::GpuGrouped32QueryRecordWire{
                .minimizer_offset = 0u,
                .minimizer_count = 1u,
                .query_length = 100u,
                .query_state_index = 0u,
                .occurrence_offset = 0u,
                .anchor_base = 0u,
                .anchor_capacity = 2u,
                .batch_join_ordinal = 101u,
            },
            gv::GpuGrouped32QueryRecordWire{
                .minimizer_offset = 1u,
                .minimizer_count = 1u,
                .query_length = 100u,
                .query_state_index = 1u,
                .occurrence_offset = 1u,
                .anchor_base = 2u,
                .anchor_capacity = 3u,
                .batch_join_ordinal = 202u,
            },
        };
        const std::array minimizers{
            gv::GpuGrouped32QueryMinimizerWire{
                .hash = 9u,
                .query_bp_strand = 28u,
                .occurrence_index = 0u,
                .query_record_index = 0u,
            },
            gv::GpuGrouped32QueryMinimizerWire{
                .hash = 11u,
                .query_bp_strand = 30u,
                .occurrence_index = 1u,
                .query_record_index = 1u,
            },
        };
        const std::array occurrences{
            gv::GpuGrouped32OccurrenceStateWire{.accepted_count = 2u},
            gv::GpuGrouped32OccurrenceStateWire{.accepted_count = 3u},
        };
        const std::array<gv::GpuGrouped32QueryStateWire, 2> query_states{};
        const std::array<gv::GpuGrouped32GlobalAnchorWire, 5> anchors{};
        const std::array<gv::GpuGrouped32AnchorRadixKeyWire, 5> keys{};
        const std::array<gv::GpuGrouped32SortSegmentWire, 2> segments{};
        gv::GpuGrouped32ValidationStateWire validation{};
        validation.completion = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::accepted);
        gv::GpuGrouped32QueryAdapterStateWire adapter{};
        adapter.completed_records = 2u;
        adapter.expected_records = 2u;
        adapter.total_minimizers = 2u;
        adapter.max_query_minimizers = 1u;
        adapter.completion = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::accepted);
        fixture.bindings[3] =
            pod_payload<gv::GpuGrouped32QueryRecordWire>(records);
        fixture.bindings[4] =
            pod_payload<gv::GpuGrouped32QueryMinimizerWire>(minimizers);
        fixture.bindings[5] =
            pod_payload<gv::GpuGrouped32OccurrenceStateWire>(occurrences);
        fixture.bindings[6] =
            pod_payload<gv::GpuGrouped32QueryStateWire>(query_states);
        fixture.bindings[7] =
            pod_payload<gv::GpuGrouped32GlobalAnchorWire>(anchors);
        fixture.bindings[8] = pod_payload(validation);
        fixture.bindings[10] =
            pod_payload<gv::GpuGrouped32AnchorRadixKeyWire>(keys);
        fixture.bindings[11] =
            pod_payload<gv::GpuGrouped32SortSegmentWire>(segments);
        fixture.bindings[12] = pod_payload(adapter);
        fixture.push.partition_count = 0u;
        fixture.push.control_count = 0u;
        fixture.push.position_count = 0u;
        fixture.push.query_record_count = 2u;
        fixture.push.query_minimizer_capacity = 2u;
        fixture.push.occurrence_count = 2u;
        fixture.push.anchor_capacity = 5u;
        fixture.push.max_partition_groups_y = 0u;
        const auto push = pod_payload(fixture.push);
        auto prefix = harness->run(
            gv::ShaderKind::phase_c_grouped32_allocate_anchor_prefix,
            push, std::move(fixture.bindings));
        REQUIRE(prefix.guards_intact);
        const auto occurrence0 = pod_from_payload<
            gv::GpuGrouped32OccurrenceStateWire>(prefix.bindings[5], 0u);
        const auto occurrence1 = pod_from_payload<
            gv::GpuGrouped32OccurrenceStateWire>(
                prefix.bindings[5], sizeof(occurrence0));
        CHECK(occurrence0.anchor_offset == 0u);
        CHECK(occurrence1.anchor_offset == 2u);
        const auto query0 = pod_from_payload<gv::GpuGrouped32QueryStateWire>(
            prefix.bindings[6], 0u);
        const auto query1 = pod_from_payload<gv::GpuGrouped32QueryStateWire>(
            prefix.bindings[6], sizeof(query0));
        CHECK(query0.anchor_count == 2u);
        CHECK(query1.anchor_count == 3u);
        const auto result = harness->run(
            gv::ShaderKind::phase_c_grouped32_build_sort_segments,
            push, std::move(prefix.bindings));
        REQUIRE(result.guards_intact);
        const auto segment0 = pod_from_payload<
            gv::GpuGrouped32SortSegmentWire>(result.bindings[11], 0u);
        const auto segment1 = pod_from_payload<
            gv::GpuGrouped32SortSegmentWire>(
                result.bindings[11], sizeof(segment0));
        CHECK(segment0.anchor_begin == 0u);
        CHECK(segment0.anchor_count == 2u);
        CHECK(segment0.query_state_index == 0u);
        CHECK(segment0.batch_join_ordinal == 101u);
        CHECK(segment1.anchor_begin == 2u);
        CHECK(segment1.anchor_count == 3u);
        CHECK(segment1.query_state_index == 1u);
        CHECK(segment1.batch_join_ordinal == 202u);
    }

    {
        INFO("forged-open gates cannot turn hostile indices into accesses");
        auto prefix_fixture = make_grouped32_core_fixture();
        auto validation = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(prefix_fixture.bindings[8]);
        validation.completion = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::accepted);
        prefix_fixture.bindings[8] = pod_payload(validation);
        auto hostile_record = pod_from_payload<
            gv::GpuGrouped32QueryRecordWire>(prefix_fixture.bindings[3]);
        hostile_record.occurrence_offset =
            (std::numeric_limits<std::uint32_t>::max)();
        prefix_fixture.bindings[3] = pod_payload(hostile_record);
        const gv::GpuGrouped32OccurrenceStateWire occurrence_sentinel{
            .global_count = 31u,
            .accepted_count = 32u,
            .anchor_offset = 33u,
            .emitted_count = 34u,
        };
        const gv::GpuGrouped32QueryStateWire query_sentinel{
            .anchor_count = 35u,
            .rejected_minimizers = 36u,
            .completed_minimizers = 37u,
            .status = 38u,
        };
        prefix_fixture.bindings[5] = pod_payload(occurrence_sentinel);
        prefix_fixture.bindings[6] = pod_payload(query_sentinel);
        const auto occurrence_before = prefix_fixture.bindings[5];
        const auto query_before = prefix_fixture.bindings[6];
        const auto prefix_push = pod_payload(prefix_fixture.push);
        const auto prefix_result = harness->run(
            gv::ShaderKind::phase_c_grouped32_allocate_anchor_prefix,
            prefix_push, std::move(prefix_fixture.bindings));
        REQUIRE(prefix_result.guards_intact);
        CHECK(prefix_result.bindings[5] == occurrence_before);
        CHECK(prefix_result.bindings[6] == query_before);
        const auto prefix_validation = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(prefix_result.bindings[8]);
        CHECK((prefix_validation.status & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::query_bounds)) != 0u);

        auto sort_fixture = make_grouped32_core_fixture();
        validation = pod_from_payload<gv::GpuGrouped32ValidationStateWire>(
            sort_fixture.bindings[8]);
        validation.completion = static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationCompletion::accepted);
        sort_fixture.bindings[8] = pod_payload(validation);
        hostile_record = pod_from_payload<gv::GpuGrouped32QueryRecordWire>(
            sort_fixture.bindings[3]);
        hostile_record.query_state_index =
            (std::numeric_limits<std::uint32_t>::max)();
        sort_fixture.bindings[3] = pod_payload(hostile_record);
        const gv::GpuGrouped32SortSegmentWire segment_sentinel{
            .anchor_begin = 39u,
            .anchor_count = 40u,
            .query_state_index = 41u,
            .flags = 42u,
            .batch_join_ordinal = 43u,
        };
        sort_fixture.bindings[11] = pod_payload(segment_sentinel);
        const auto segment_before = sort_fixture.bindings[11];
        const auto sort_push = pod_payload(sort_fixture.push);
        const auto sort_result = harness->run(
            gv::ShaderKind::phase_c_grouped32_build_sort_segments,
            sort_push, std::move(sort_fixture.bindings));
        REQUIRE(sort_result.guards_intact);
        CHECK(sort_result.bindings[11] == segment_before);
        const auto sort_validation = pod_from_payload<
            gv::GpuGrouped32ValidationStateWire>(sort_result.bindings[8]);
        CHECK((sort_validation.status & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::query_bounds)) != 0u);
    }

    const std::uint64_t total_dispatch_calls =
        harness->total_dispatch_calls();
    const std::uint64_t total_workgroups_x =
        harness->total_dispatched_workgroups_x();
    CAPTURE(total_dispatch_calls, total_workgroups_x);
    REQUIRE(total_dispatch_calls > 0u);
    REQUIRE(total_workgroups_x >= total_dispatch_calls);
    CAPTURE(harness->device_name(), harness->device_type());
    REQUIRE(harness->device_type() != VK_PHYSICAL_DEVICE_TYPE_CPU);
    std::cout << "grouped32_vulkan_execution_metrics"
              << " device_name=\"" << harness->device_name() << '"'
              << " device_type=" << static_cast<std::uint32_t>(
                     harness->device_type())
              << " total_dispatch_calls=" << total_dispatch_calls
              << " total_workgroups_x=" << total_workgroups_x;
    for (const auto& [kind, ignored_spirv] : structural_modules) {
        (void)ignored_spirv;
        const std::uint64_t calls = harness->dispatch_calls(kind);
        const std::uint64_t workgroups =
            harness->dispatched_workgroups_x(kind);
        const auto key = gv::make_shader_key(kind, 384u, 128u, 2u);
        CAPTURE(gv::cache_file_stem(key), calls, workgroups);
        CHECK(calls > 0u);
        CHECK(workgroups >= calls);
        std::cout << ' ' << gv::cache_file_stem(key)
                  << "_calls=" << calls
                  << "_workgroups_x=" << workgroups;
    }
    std::cout << '\n';
}

TEST_CASE("format-4 local32 positions globalize beyond 4 Gbp on both strands",
          "[grouped32][abi2][oracle]") {
    constexpr std::uint64_t base = (std::uint64_t{1} << 32u) + 1234u;
    const gv::GpuGrouped32PartitionWire partition{
        .partition_id = 0u,
        .position_segment = 2u,
        .control_offset = 0u,
        .control_count = 1u,
        .position_offset = 0u,
        .position_count = 2u,
        .low_hash = 7u,
        .high_hash = 7u,
        .segment_base_bp = base,
        .segment_end_bp = base + 100u,
    };
    const auto forward = grouped32_globalize_oracle(
        partition,
        gv::GpuGrouped32PositionWire{.local_bp_strand = 17u << 1u});
    const auto reverse = grouped32_globalize_oracle(
        partition,
        gv::GpuGrouped32PositionWire{
            .local_bp_strand = (18u << 1u) | 1u});
    REQUIRE(forward);
    REQUIRE(reverse);
    CHECK(forward->bp == base + 17u);
    CHECK(forward->strand == 0u);
    CHECK(reverse->bp == base + 18u);
    CHECK(reverse->strand == 1u);
    CHECK(forward->bp > (std::numeric_limits<std::uint32_t>::max)());
}

TEST_CASE("format-4 max-occ is finalized once from the target-global sum",
          "[grouped32][abi2][oracle][max-occ]") {
    constexpr std::uint32_t max_occ = 5u;
    constexpr std::array segment_counts{3u, 4u};
    STATIC_REQUIRE(segment_counts[0] <= max_occ);
    STATIC_REQUIRE(segment_counts[1] <= max_occ);

    std::uint32_t global = 0u;
    for (const std::uint32_t segment_count : segment_counts) {
        global = grouped32_saturating_count_oracle(
            global, segment_count, max_occ);
    }
    CHECK(global == max_occ + 1u);
    CHECK(grouped32_finalize_count_oracle(global, max_occ) == 0u);
}

TEST_CASE("format-4 structural oracle rejects corrupt offsets and counts",
          "[grouped32][abi2][oracle][validation]") {
    const gv::GpuGrouped32PartitionWire partition{
        .partition_id = 0u,
        .position_segment = 0u,
        .control_offset = 0u,
        .control_count = 2u,
        .position_offset = 0u,
        .position_count = 3u,
        .low_hash = 10u,
        .high_hash = 20u,
        .segment_base_bp = 0u,
        .segment_end_bp = 100u,
    };
    const std::array valid_controls{
        gv::GpuGrouped32ControlWire{
            .hash = 10u, .position_offset = 0u, .position_count = 2u},
        gv::GpuGrouped32ControlWire{
            .hash = 20u, .position_offset = 2u, .position_count = 1u},
    };
    const std::array valid_positions{
        gv::GpuGrouped32PositionWire{.local_bp_strand = 4u},
        gv::GpuGrouped32PositionWire{.local_bp_strand = 8u},
        gv::GpuGrouped32PositionWire{.local_bp_strand = 12u},
    };
    CHECK(grouped32_partition_validation_oracle(
        partition, valid_controls, valid_positions) == 0u);

    auto corrupt_offset = valid_controls;
    corrupt_offset[1].position_offset = 1u;
    CHECK((grouped32_partition_validation_oracle(
        partition, corrupt_offset, valid_positions)
        & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::control_positions)) != 0u);

    auto corrupt_count = valid_controls;
    corrupt_count[1].position_count = 2u;
    CHECK((grouped32_partition_validation_oracle(
        partition, corrupt_count, valid_positions)
        & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::control_positions)) != 0u);

    auto corrupt_descriptor = partition;
    corrupt_descriptor.control_offset = 2u;
    CHECK((grouped32_partition_validation_oracle(
        corrupt_descriptor, valid_controls, valid_positions)
        & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::descriptor_bounds)) != 0u);
}

TEST_CASE("grouped32 directory binds gapped stable IDs to dense ordinals",
          "[grouped32][abi2][oracle][identity]") {
    std::array partitions{
        gv::GpuGrouped32PartitionWire{
            .partition_id = 3u, .directory_ordinal = 0u},
        gv::GpuGrouped32PartitionWire{
            .partition_id = 17u, .directory_ordinal = 1u},
    };
    CHECK(grouped32_directory_identity_oracle(partitions) == 0u);

    auto duplicate = partitions;
    duplicate[1].partition_id = 3u;
    CHECK(grouped32_directory_identity_oracle(duplicate) != 0u);
    auto wrong_ordinal = partitions;
    wrong_ordinal[1].directory_ordinal = 17u;
    CHECK(grouped32_directory_identity_oracle(wrong_ordinal) != 0u);
}

TEST_CASE("grouped32 rejects duplicate positions and malformed q_last",
          "[grouped32][abi2][oracle][adversarial]") {
    const gv::GpuGrouped32PartitionWire partition{
        .partition_id = 3u,
        .directory_ordinal = 0u,
        .position_segment = 0u,
        .control_offset = 0u,
        .control_count = 1u,
        .position_offset = 0u,
        .position_count = 2u,
        .low_hash = 9u,
        .high_hash = 9u,
        .segment_base_bp = 0u,
        .segment_end_bp = 100u,
    };
    const std::array controls{gv::GpuGrouped32ControlWire{
        .hash = 9u, .position_offset = 0u, .position_count = 2u}};
    const std::array duplicate_positions{
        gv::GpuGrouped32PositionWire{.local_bp_strand = 8u},
        gv::GpuGrouped32PositionWire{.local_bp_strand = 8u},
    };
    CHECK((grouped32_partition_validation_oracle(
        partition, controls, duplicate_positions)
        & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::position_order)) != 0u);

    const gv::GpuGrouped32QueryRecordWire record{
        .minimizer_offset = 0u,
        .minimizer_count = 1u,
        .query_length = 100u,
        .query_state_index = 0u,
    };
    CHECK_FALSE(grouped32_query_encoding_oracle(
        record, gv::GpuGrouped32QueryMinimizerWire{
            .query_bp_strand = 13u << 1u}));
    CHECK(grouped32_query_encoding_oracle(
        record, gv::GpuGrouped32QueryMinimizerWire{
            .query_bp_strand = 14u << 1u}));
    auto over_31_bit_query = record;
    over_31_bit_query.query_length = 0x80000001u;
    CHECK_FALSE(grouped32_query_encoding_oracle(
        over_31_bit_query, gv::GpuGrouped32QueryMinimizerWire{
            .query_bp_strand = 14u << 1u}));
}

TEST_CASE("grouped32 validation gate remains closed until both tokens accept",
          "[grouped32][abi2][oracle][gate]") {
    using Completion = gv::GpuGrouped32ValidationCompletion;
    gv::GpuGrouped32ValidationStateWire validation{};
    gv::GpuGrouped32QueryAdapterStateWire adapter{};
    CHECK_FALSE(grouped32_gate_oracle(validation, adapter));
    validation.completion = static_cast<std::uint32_t>(Completion::accepted);
    CHECK_FALSE(grouped32_gate_oracle(validation, adapter));
    adapter.completion = static_cast<std::uint32_t>(Completion::accepted);
    CHECK(grouped32_gate_oracle(validation, adapter));
    validation.status = 1u;
    CHECK_FALSE(grouped32_gate_oracle(validation, adapter));

}

TEST_CASE("grouped32 typed barriers require indirect and every stage dependency",
          "[grouped32][abi2][barrier][proof]") {
    gv::GpuGrouped32DeviceIdentity device{};
    device.device_uuid[0] = 0x42u;
    device.device_kind = gv::GpuGrouped32PhysicalDeviceKind::integrated_gpu;
    device.vendor_id = 1u;
    device.device_id = 2u;
    device.driver_version = 3u;

    gv::GpuGrouped32ExecutionIdentity execution{};
    execution.generation_id = 10u;
    execution.source_identity = 11u;
    execution.source_content_checksum = 12u;
    execution.compatibility_checksum = 13u;
    execution.cache_identity = 14u;
    execution.resident_identity = 15u;
    execution.query_identity = 16u;
    execution.params_identity = 17u;
    execution.submission_identity = 18u;
    execution.fence_identity = 19u;
    execution.fence_value = 20u;
    execution.device = device;

    gv::GpuGrouped32ResidentIdentity resident{};
    resident.generation_id = execution.generation_id;
    resident.source_identity = execution.source_identity;
    resident.source_content_checksum = execution.source_content_checksum;
    resident.compatibility_checksum = execution.compatibility_checksum;
    resident.cache_identity = execution.cache_identity;
    resident.resident_identity = execution.resident_identity;
    resident.submission_identity = 21u;
    resident.fence_identity = 22u;
    resident.fence_value = 23u;
    resident.device = device;
    CHECK(gv::gpu_grouped32_execution_identity_bound(execution, resident));

    gv::GpuGrouped32BarrierPlan plan{};
    plan.record_count = static_cast<std::uint32_t>(
        gv::k_gpu_grouped32_required_barrier_count);
    for (std::size_t i = 0u;
         i < gv::k_gpu_grouped32_required_barrier_count; ++i) {
        const auto& required = gv::k_gpu_grouped32_required_barriers[i];
        auto& record = plan.records[i];
        record.source_stage = required.source_stage;
        record.destination_stage = required.destination_stage;
        record.source_access = required.source_access;
        record.destination_access = required.destination_access;
        record.source_pipeline_scope =
            gv::GpuGrouped32PipelineScope::compute_shader;
        record.destination_pipeline_scope =
            gv::GpuGrouped32PipelineScope::compute_shader;
        if (gv::gpu_grouped32_has_barrier_access(
                required.destination_access,
                gv::GpuGrouped32BarrierAccess::indirect_command_read)) {
            record.destination_pipeline_scope =
                record.destination_pipeline_scope
                | gv::GpuGrouped32PipelineScope::draw_indirect;
        }
        record.submission_identity = execution.submission_identity;
        record.fence_identity = execution.fence_identity;
        record.fence_value = execution.fence_value;
        record.device = device;
    }
    CHECK(gv::gpu_grouped32_barrier_plan_valid(plan, execution));
    CHECK(plan.records[0].source_stage
        == gv::GpuGrouped32ExecutionStage::state_clear);
    CHECK(plan.records[0].destination_stage
        == gv::GpuGrouped32ExecutionStage::query_sketch);
    CHECK(plan.records[2].source_stage
        == gv::GpuGrouped32ExecutionStage::query_adapter);
    CHECK(plan.records[2].destination_stage
        == gv::GpuGrouped32ExecutionStage::query_adapter_finalize);
    CHECK(plan.records[5].source_stage
        == gv::GpuGrouped32ExecutionStage::resident_validation);
    CHECK(plan.records[5].destination_stage
        == gv::GpuGrouped32ExecutionStage::validation_finalize);
    CHECK(gv::gpu_grouped32_has_barrier_access(
        plan.records[2].source_access,
        gv::GpuGrouped32BarrierAccess::compute_shader_write));
    CHECK(gv::gpu_grouped32_has_barrier_access(
        plan.records[2].destination_access,
        gv::GpuGrouped32BarrierAccess::compute_shader_read));
    CHECK(gv::gpu_grouped32_has_barrier_access(
        plan.records[5].source_access,
        gv::GpuGrouped32BarrierAccess::compute_shader_write));
    CHECK(gv::gpu_grouped32_has_barrier_access(
        plan.records[5].destination_access,
        gv::GpuGrouped32BarrierAccess::compute_shader_read));
    CHECK(gv::gpu_grouped32_has_barrier_access(
        plan.records[4].destination_access,
        gv::GpuGrouped32BarrierAccess::indirect_command_read));
    CHECK(gv::gpu_grouped32_has_pipeline_scope(
        plan.records[4].destination_pipeline_scope,
        gv::GpuGrouped32PipelineScope::draw_indirect));
    CHECK(gv::gpu_grouped32_has_barrier_access(
        plan.records[6].destination_access,
        gv::GpuGrouped32BarrierAccess::indirect_command_read));
    CHECK(gv::gpu_grouped32_has_pipeline_scope(
        plan.records[6].destination_pipeline_scope,
        gv::GpuGrouped32PipelineScope::draw_indirect));

    auto missing_indirect = plan;
    missing_indirect.records[4].destination_access =
        gv::GpuGrouped32BarrierAccess::compute_shader_read;
    CHECK_FALSE(gv::gpu_grouped32_barrier_plan_valid(
        missing_indirect, execution));
    auto missing_indirect_scope = plan;
    missing_indirect_scope.records[4].destination_pipeline_scope =
        gv::GpuGrouped32PipelineScope::compute_shader;
    CHECK_FALSE(gv::gpu_grouped32_barrier_plan_valid(
        missing_indirect_scope, execution));
    auto wrong_fence = plan;
    wrong_fence.records[6].fence_value += 1u;
    CHECK_FALSE(gv::gpu_grouped32_barrier_plan_valid(wrong_fence, execution));
    auto missing_dependency = plan;
    missing_dependency.record_count -= 1u;
    CHECK_FALSE(gv::gpu_grouped32_barrier_plan_valid(
        missing_dependency, execution));

    gv::GpuGrouped32StageExecutionRecord radix{};
    radix.stage = gv::GpuGrouped32ExecutionStage::radix_sort;
    radix.submission_identity = execution.submission_identity;
    radix.fence_identity = execution.fence_identity;
    radix.fence_value = execution.fence_value;
    radix.device = device;
    CHECK_FALSE(gv::gpu_grouped32_stage_record_matches(
        radix, gv::GpuGrouped32ExecutionStage::radix_sort, execution));
    radix.dispatch_count = 1u;
    CHECK(gv::gpu_grouped32_stage_record_matches(
        radix, gv::GpuGrouped32ExecutionStage::radix_sort, execution));
    for (std::uint32_t raw = 0u;
         raw < static_cast<std::uint32_t>(
             gv::GpuGrouped32ExecutionStage::count); ++raw) {
        auto stage = radix;
        stage.stage = static_cast<gv::GpuGrouped32ExecutionStage>(raw);
        CHECK(gv::gpu_grouped32_stage_record_matches(
            stage, stage.stage, execution));
    }
    auto cpu_execution = execution;
    cpu_execution.device.device_kind =
        gv::GpuGrouped32PhysicalDeviceKind::cpu;
    CHECK_FALSE(gv::gpu_grouped32_execution_identity_bound(
        cpu_execution, resident));
}

TEST_CASE("grouped32 keeps non-dense u64 BatchJoin ordinals and poisons moves",
          "[grouped32][abi2][proof][identity]") {
    constexpr std::uint64_t first = (std::uint64_t{1} << 40u) + 7u;
    constexpr std::uint64_t second = (std::uint64_t{1} << 52u) + 9u;
    gv::GpuGrouped32QueryRecordWire record{};
    record.query_state_index = 0u;
    record.batch_join_ordinal = second;
    gv::GpuGrouped32SortSegmentWire segment{};
    segment.query_state_index = record.query_state_index;
    segment.batch_join_ordinal = record.batch_join_ordinal;
    gv::GpuGrouped32FinalResultCigarWire result{};
    result.batch_join_ordinal = segment.batch_join_ordinal;
    CHECK(first < record.batch_join_ordinal);
    CHECK(result.batch_join_ordinal == second);

    auto resident =
        gv::GpuGrouped32ValidatedResidentProof::rejected_test_token();
    auto moved_resident = std::move(resident);
    CHECK(resident.moved_from());
    CHECK_FALSE(resident.accepted());
    CHECK_FALSE(moved_resident.accepted());
    auto foundation =
        gv::GpuGrouped32FoundationOnlyProof::rejected_test_token();
    auto moved_foundation = std::move(foundation);
    CHECK(foundation.moved_from());
    CHECK_FALSE(foundation.foundation_validated());
    CHECK_FALSE(moved_foundation.foundation_validated());
    STATIC_REQUIRE_FALSE(
        gv::GpuGrouped32FoundationOnlyProof::qualifies_as_all_gpu_execution());
    auto execution = gv::GpuGrouped32ExecutionProof::rejected_test_token();
    auto moved_execution = std::move(execution);
    CHECK(execution.moved_from());
    CHECK_FALSE(execution.accepted());
    CHECK_FALSE(moved_execution.accepted());

    gv::GpuGrouped32DeviceIdentity device{};
    device.device_uuid[0] = 1u;
    device.device_kind = gv::GpuGrouped32PhysicalDeviceKind::cpu;
    CHECK_FALSE(gv::gpu_grouped32_hardware_device(device));
    device.device_kind = gv::GpuGrouped32PhysicalDeviceKind::integrated_gpu;
    CHECK(gv::gpu_grouped32_hardware_device(device));
}

TEST_CASE("format-4 target-wide sort input crosses partition boundaries",
          "[grouped32][abi2][oracle][ordering]") {
    constexpr std::uint64_t high_base =
        (std::uint64_t{1} << 32u) + 1000u;
    struct TaggedAnchor {
        gv::GpuGrouped32GlobalAnchorWire anchor{};
        std::uint32_t source_partition = 0u;
    };
    std::vector<TaggedAnchor> anchors{
        TaggedAnchor{
            .anchor = gv::GpuGrouped32GlobalAnchorWire{
                .ref_position = high_base + 9u,
                .query_position = 30u,
                .is_reverse = 0u,
                .query_span = 15u,
            },
            .source_partition = 4u,
        },
        TaggedAnchor{
            .anchor = gv::GpuGrouped32GlobalAnchorWire{
                .ref_position = 77u,
                .query_position = 12u,
                .is_reverse = 0u,
                .query_span = 15u,
            },
            .source_partition = 1u,
        },
        TaggedAnchor{
            .anchor = gv::GpuGrouped32GlobalAnchorWire{
                .ref_position = 33u,
                .query_position = 8u,
                .is_reverse = 1u,
                .query_span = 15u,
            },
            .source_partition = 0u,
        },
    };
    std::sort(anchors.begin(), anchors.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.anchor.is_reverse != rhs.anchor.is_reverse) {
            return lhs.anchor.is_reverse < rhs.anchor.is_reverse;
        }
        if (lhs.anchor.ref_position != rhs.anchor.ref_position) {
            return lhs.anchor.ref_position < rhs.anchor.ref_position;
        }
        return lhs.anchor.query_position < rhs.anchor.query_position;
    });

    REQUIRE(anchors.size() == 3u);
    CHECK(anchors[0].source_partition == 1u);
    CHECK(anchors[1].source_partition == 4u);
    CHECK(anchors[2].source_partition == 0u);
    CHECK(anchors[1].anchor.ref_position >
          (std::numeric_limits<std::uint32_t>::max)());
}

TEST_CASE("DOC0371 persisted directory selector shader compiles with exact bindings",
          "[shader_cache][doc0371][persisted-directory]") {
    const auto key = gv::make_shader_key(
        gv::ShaderKind::phase_c_persisted_directory_select,
        384u, 128u, 1u);
    REQUIRE(gv::valid_shader_variant_key(key));
    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    const auto layout = gv::validate_spirv_layout(key, spirv);
    REQUIRE(layout.has_value());
}

TEST_CASE("Phase-B seed lookup AOT shader pins six-buffer key-emission ABI",
          "[shader_cache][phase-b][seed-sort]") {
    const auto key = gv::make_shader_key(
        gv::ShaderKind::seed_lookup_anchors, 384u, 128u, 256u);
    REQUIRE(gv::valid_shader_variant_key(key));
    REQUIRE(gv::descriptor_binding_count(
        gv::DescriptorLayoutKind::seed_lookup) == 6u);
    REQUIRE(gv::push_constant_size(
        gv::ShaderKind::seed_lookup_anchors) == 44u);

    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    REQUIRE(gv::validate_spirv_layout(key, spirv));
}

TEST_CASE("score-only affine extension has one provider-compatible AOT variant",
          "[shader_cache][alignment-extension][no-fallback]") {
    const auto key = gv::make_shader_key(
        gv::ShaderKind::align_extend_banded_affine, 384u, 1u, 512u);
    REQUIRE(gv::valid_shader_variant_key(key));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::ShaderKind::align_extend_banded_affine);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::ShaderWorkgroupSize{1u, 1u, 1u}));
    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    REQUIRE(gv::validate_spirv_layout(key, spirv));
    const auto msl = gv::spirv_to_msl(spirv, key, gv::MslVersion{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 1u);
    CHECK(msl->wg_y == 1u);
    CHECK(msl->wg_z == 1u);
}

TEST_CASE("fixed radix workgroups have one authoritative cache ABI",
          "[shader_cache][workgroup-abi]") {
    const std::array kinds{
        gv::ShaderKind::phase_c_sort_radix_histogram,
        gv::ShaderKind::phase_c_sort_radix_scan,
        gv::ShaderKind::phase_c_sort_radix_scatter,
    };
    for (const gv::ShaderKind kind : kinds) {
        const gv::ShaderVariantKey key =
            gv::make_shader_key(kind, 384u, 1u, 1u);
        CAPTURE(gv::cache_file_stem(key));
        REQUIRE(gv::valid_shader_variant_key(key));

        // The established key uses local_size=1 as a selector, while the
        // algorithm has one invocation per radix bin. Reflection, validation,
        // and archive selection must agree on the compiled 256-lane geometry.
        const gv::ShaderWorkgroupSize expected =
            gv::expected_shader_workgroup_size(key);
        REQUIRE(expected == (gv::ShaderWorkgroupSize{256u, 1u, 1u}));

        const auto spirv = gv::compile_shader_spirv(key.to_request());
        REQUIRE_FALSE(spirv.empty());
        const auto msl = gv::spirv_to_msl(spirv, key, gv::MslVersion{});
        REQUIRE(msl);
        CHECK(msl->wg_x == expected.x);
        CHECK(msl->wg_y == expected.y);
        CHECK(msl->wg_z == expected.z);
    }
}

TEST_CASE("DOC0341 shader cache digest changes with variant ABI inputs",
          "[shader_cache][doc0341]") {
    gv::ShaderCacheBuildInputs inputs{};
    inputs.variants = gv::VulkanStaticVariantInputs{
        .max_workgroup_invocations = 256,
        .max_shared_memory_bytes = 32u * 1024u,
        .default_window_size = 10,
    };
    inputs.moltenvk_build = "unit-test-moltenvk";
    inputs.device_uuid = "unit-test-device";

    const std::string digest_a = gv::shader_cache_digest_hex(inputs);
    const std::string digest_b = gv::shader_cache_digest_hex(inputs);
    REQUIRE(is_hex_digest(digest_a));
    CHECK(digest_a == digest_b);

    inputs.variants.default_window_size = 11;
    const std::string digest_c = gv::shader_cache_digest_hex(inputs);
    REQUIRE(is_hex_digest(digest_c));
    CHECK(digest_c != digest_a);
}

TEST_CASE("DOC0341 shader cache meta and MoltenVK header payload are exact",
          "[shader_cache][doc0341]") {
    TempDir temp;
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    constexpr std::string_view entry = "main0";

    gv::ShaderCacheMeta meta{};
    meta.key = key;
    meta.wg_x = 256u;
    meta.wg_y = 1u;
    meta.wg_z = 1u;
    meta.entry_len = static_cast<std::uint32_t>(entry.size());

    const std::vector<std::byte> meta_bytes = gv::meta_to_bytes(meta, entry);
    auto meta_write = gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(meta_bytes.data(), meta_bytes.size()));
    REQUIRE(meta_write);

    const std::vector<std::byte> metallib = bytes_of("not-a-real-metallib");
    auto metallib_write = gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key),
        std::span<const std::byte>(metallib.data(), metallib.size()));
    REQUIRE(metallib_write);

    // cache_entry_complete also requires a non-empty .metalar binary archive on Apple.
    const std::vector<std::byte> fake_archive = bytes_of("not-a-real-metalar");
    auto archive_write = gv::durable_write_file(
        gv::cache_binary_archive_path(temp.path, key),
        std::span<const std::byte>(fake_archive.data(), fake_archive.size()));
    REQUIRE(archive_write);
    REQUIRE(gv::cache_entry_complete(temp.path, key));

    std::string decoded_entry;
    const auto decoded_meta = gv::read_meta(
        gv::cache_meta_path(temp.path, key), &decoded_entry);
    REQUIRE(decoded_meta);
    CHECK(decoded_meta->key == key);
    CHECK(decoded_meta->wg_x == 256u);
    CHECK(decoded_entry == entry);

    // A cache entry with the right key but a different compiled workgroup ABI
    // is not reusable.  This is the local compatibility check complementing
    // the digest-level resource-binding ABI revision.
    gv::ShaderCacheMeta incompatible_meta = meta;
    incompatible_meta.wg_x = 128u;
    const std::vector<std::byte> incompatible_meta_bytes =
        gv::meta_to_bytes(incompatible_meta, entry);
    auto incompatible_meta_write = gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(
            incompatible_meta_bytes.data(), incompatible_meta_bytes.size()));
    REQUIRE(incompatible_meta_write);
    CHECK_FALSE(gv::cache_metallib_entry_complete(temp.path, key));
    CHECK_FALSE(gv::cache_entry_complete(temp.path, key));

    shader_module_create_called = false;
    const auto loader_rejected = gv::load_cached_module(
        VK_NULL_HANDLE, temp.path, key, &record_shader_module_create);
    REQUIRE_FALSE(loader_rejected);
    CHECK(loader_rejected.error().find("meta workgroup mismatch")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    NoPublishCompileService no_publish_service{};
    const auto post_compile_rejected = gv::load_module(
        VK_NULL_HANDLE,
        temp.path,
        key,
        &record_shader_module_create,
        no_publish_service);
    REQUIRE_FALSE(post_compile_rejected);
    CHECK(post_compile_rejected.error().find("complete compatible cache entry")
          != std::string::npos);
    CHECK(post_compile_rejected.error().find(
              "metadata incompatible: meta workgroup mismatch")
          != std::string::npos);
    CHECK(no_publish_service.calls == 1);
    CHECK_FALSE(shader_module_create_called);

    const auto module_code = gv::make_moltenvk_metallib_module_code(
        gv::cache_metallib_path(temp.path, key));
    REQUIRE(module_code);
    CHECK(module_code->header_size_bytes == gv::moltenvk_msl_spirv_header_size());
    CHECK(module_code->code_size_bytes
          == gv::moltenvk_msl_spirv_header_size() + metallib.size());
    CHECK(reinterpret_cast<std::uintptr_t>(module_code->words.data())
          % alignof(std::uint32_t) == 0u);

    const auto* submitted =
        reinterpret_cast<const std::byte*>(module_code->words.data());
    REQUIRE(module_code->header_size_bytes >= sizeof(std::uint32_t));
    std::uint32_t magic = 0;
    std::memcpy(&magic, submitted, sizeof(magic));
    CHECK(magic == gv::moltenvk_msl_compiled_code_magic());
    CHECK(std::memcmp(
              submitted + module_code->header_size_bytes,
              metallib.data(),
              metallib.size()) == 0);
}

TEST_CASE("fixed radix cache accepts reflected workgroup and exact archive",
          "[shader_cache][workgroup-abi]") {
    TempDir temp;
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::phase_c_sort_radix_histogram, 384u, 1u, 1u);
    constexpr std::string_view entry = "main0";

    gv::ShaderCacheMeta meta{};
    meta.key = key;
    meta.wg_x = 256u;
    meta.wg_y = 1u;
    meta.wg_z = 1u;
    meta.entry_len = static_cast<std::uint32_t>(entry.size());
    const std::vector<std::byte> meta_bytes = gv::meta_to_bytes(meta, entry);
    REQUIRE(gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(meta_bytes.data(), meta_bytes.size())));

    const std::vector<std::byte> metallib = bytes_of("fixed-radix-metallib");
    REQUIRE(gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key),
        std::span<const std::byte>(metallib.data(), metallib.size())));
    const std::vector<std::byte> archive = bytes_of("fixed-radix-metalar");
    REQUIRE(gv::durable_write_file(
        gv::cache_binary_archive_path(temp.path, key),
        std::span<const std::byte>(archive.data(), archive.size())));

    // Publication uses reflected threads, so a 256-thread shader needs only
    // the exact base archive; completeness must not invent a duplicate
    // `.max_threads_256.metalar` from the key's sentinel local_size.
    CHECK(gv::cache_binary_archive_paths(temp.path, key).size() == 1u);
    REQUIRE(gv::cache_entry_status(temp.path, key));
    REQUIRE(gv::cache_entry_complete(temp.path, key));

    shader_module_create_called = false;
    NoPublishCompileService should_not_compile{};
    const auto loaded = gv::load_module(
        VK_NULL_HANDLE,
        temp.path,
        key,
        &record_shader_module_create,
        should_not_compile);
    REQUIRE(loaded);
    CHECK(loaded->wg_x == 256u);
    CHECK(should_not_compile.calls == 0);
    CHECK(shader_module_create_called);
}

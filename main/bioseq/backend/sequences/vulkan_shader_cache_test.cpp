#include <tests/support/common.hpp>
#include "common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
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

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.sequences.prefilters.prefilter_types;

namespace {

namespace gv = bioseq::gpu::vulkan;
namespace pf = bioseq::sequences::prefilters;

bool shader_module_create_called = false;
bool shader_module_payload_matches = false;
std::vector<std::byte> expected_shader_module_metallib;
std::filesystem::path seal_identity_mutation_path;
bool seal_identity_mutation_succeeded = false;

void mutate_seal_artifact_same_size() noexcept {
    seal_identity_mutation_succeeded = false;
    const int fd = ::open(
        seal_identity_mutation_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    std::byte value{};
    if (::pread(fd, &value, 1u, 0) == 1) {
        value ^= std::byte{0x01u};
        seal_identity_mutation_succeeded =
            ::pwrite(fd, &value, 1u, 0) == 1 && ::fsync(fd) == 0;
    }
    (void)::close(fd);
}

VKAPI_ATTR VkResult VKAPI_CALL record_shader_module_create(
    VkDevice,
    const VkShaderModuleCreateInfo*,
    const VkAllocationCallbacks*,
    VkShaderModule*)
{
    shader_module_create_called = true;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL record_exact_shader_module_create(
    VkDevice,
    const VkShaderModuleCreateInfo* create_info,
    const VkAllocationCallbacks*,
    VkShaderModule*)
{
    shader_module_create_called = true;
    const std::size_t header_bytes = gv::moltenvk_msl_spirv_header_size();
    shader_module_payload_matches = create_info != nullptr
        && create_info->pCode != nullptr
        && create_info->codeSize
            == header_bytes + expected_shader_module_metallib.size()
        && std::memcmp(
            reinterpret_cast<const std::byte*>(create_info->pCode)
                + header_bytes,
            expected_shader_module_metallib.data(),
            expected_shader_module_metallib.size()) == 0;
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

struct ScopedEnvironmentVariable {
    std::string name;
    std::optional<std::string> previous;

    ScopedEnvironmentVariable(std::string variable, std::string_view value)
        : name(std::move(variable))
    {
        if (const char* existing = std::getenv(name.c_str()); existing != nullptr) {
            previous = existing;
        }
        REQUIRE(::setenv(name.c_str(), std::string(value).c_str(), 1) == 0);
    }

    ~ScopedEnvironmentVariable()
    {
        if (previous) {
            (void)::setenv(name.c_str(), previous->c_str(), 1);
        } else {
            (void)::unsetenv(name.c_str());
        }
    }
};

struct ScopedArchiveHelperStoreReset {
    ScopedArchiveHelperStoreReset() {
        REQUIRE(gv::qualified_metal_archive_helper_process_store()
                    .reset_for_test());
    }
    ~ScopedArchiveHelperStoreReset() {
        (void)gv::qualified_metal_archive_helper_process_store()
            .reset_for_test();
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

[[nodiscard]] std::vector<std::byte> read_file_bytes(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    REQUIRE(size >= 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    REQUIRE(input);
    return bytes;
}

void write_fake_archive_qualification(
    const std::filesystem::path& archive,
    std::span<const std::byte> archive_bytes,
    const gv::ShaderVariantKey& key,
    std::span<const std::byte> metallib_bytes,
    std::string_view entry_name,
    std::uint64_t max_threads = 256u)
{
#if defined(__APPLE__)
    const auto helper =
        gv::qualified_metal_archive_helper_process_store().load_once();
    REQUIRE(helper);
    const std::string helper_source = (**helper).source;
    const std::uint64_t helper_size = (**helper).executable_size;
    const std::array<std::uint8_t, 32> helper_sha256 =
        (**helper).executable_sha256;
#else
    const auto portable_helper = bytes_of("portable-helper-fixture");
    const std::string helper_source = "installed-sibling";
    const std::uint64_t helper_size =
        static_cast<std::uint64_t>(portable_helper.size());
    const std::array<std::uint8_t, 32> helper_sha256 =
        gv::sealed_shader_cache_detail::sha256(portable_helper);
#endif
    const gv::MetalArchiveQualificationReceipt qualification{
        .wire_version =
            gv::sealed_shader_cache_detail::archive_qualification_version,
        .shader_key = key,
        .metallib_size = static_cast<std::uint64_t>(metallib_bytes.size()),
        .metallib_sha256 =
            gv::sealed_shader_cache_detail::sha256(metallib_bytes),
        .entry_name = std::string(entry_name),
        .max_total_threads_per_threadgroup = max_threads,
        .threadgroup_size_is_multiple = false,
        .negative_error = gv::MetalArchiveNegativeErrorFingerprint{
            .domain = "MTLLibraryErrorDomain",
            .code = 6,
        },
        .archive_size = static_cast<std::uint64_t>(archive_bytes.size()),
        .archive_sha256 =
            gv::sealed_shader_cache_detail::sha256(archive_bytes),
        .helper_protocol_version = 2u,
        .helper_build_identity = "bioseq_gpu_archive_builder_v2",
        .helper_source = helper_source,
        .helper_executable_size = helper_size,
        .helper_executable_sha256 = helper_sha256,
        .helper_metal_registry_id = 0x1234u,
    };
    const auto qualification_bytes = gv::sealed_shader_cache_detail::
        serialize_metal_archive_qualification_receipt(qualification);
    REQUIRE(qualification_bytes);
    REQUIRE(gv::durable_write_file(
        gv::cache_binary_archive_qualification_path(archive),
        *qualification_bytes));
}

void write_fake_sealed_shader_artifacts(
    const std::filesystem::path& cache_dir,
    std::span<const gv::ShaderVariantKey> keys)
{
    constexpr std::string_view entry = "main0";
    for (const gv::ShaderVariantKey& key : keys) {
        const gv::ShaderWorkgroupSize workgroup =
            gv::expected_shader_workgroup_size(key);
        gv::ShaderCacheMeta meta{};
        meta.key = key;
        meta.wg_x = workgroup.x;
        meta.wg_y = workgroup.y;
        meta.wg_z = workgroup.z;
        meta.entry_len = static_cast<std::uint32_t>(entry.size());
        const auto meta_bytes = gv::meta_to_bytes(meta, entry);
        REQUIRE(gv::durable_write_file(
            gv::cache_meta_path(cache_dir, key), meta_bytes));
        const auto metallib = bytes_of(
            "test-metallib-" + gv::cache_file_stem(key));
        REQUIRE(gv::durable_write_file(
            gv::cache_metallib_path(cache_dir, key), metallib));
        const auto archives = gv::cache_binary_archive_paths(cache_dir, key);
        const std::uint64_t exact_threads =
            static_cast<std::uint64_t>(workgroup.x)
            * static_cast<std::uint64_t>(workgroup.y)
            * static_cast<std::uint64_t>(workgroup.z);
        for (std::size_t archive_ordinal = 0u;
             archive_ordinal < archives.size(); ++archive_ordinal) {
            const std::filesystem::path& archive = archives[archive_ordinal];
            const auto archive_bytes = bytes_of(
                "qualified-test-archive-" + archive.filename().string());
            REQUIRE(gv::durable_write_file(archive, archive_bytes));
            write_fake_archive_qualification(
                archive, archive_bytes, key, metallib, entry,
                archive_ordinal == 0u ? exact_threads : 256u);
        }
    }
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

    REQUIRE(keys.size() == 44);
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
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_shape_v5_seed_count, 384u, 128u, 3u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_shape_v5_seed_emit, 384u, 128u, 3u)));
    CHECK_FALSE(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 128u, 2u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_banded_affine, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_simple_cigar, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_simple_cigar, 384u, 64u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::sneaky_snake_rank_batch, 384u, 64u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::gatekeeper_rank_batch, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::gatekeeper_rank_batch_l32_e2, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::gatekeeper_rank_batch_l32_e4, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::gatekeeper_rank_batch_l64_e2, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::gatekeeper_rank_batch_l64_e4, 384u, 64u, 128u)));
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

TEST_CASE("format-5 ABI3 direct seed shaders pin raw SoA and local-position ABI",
          "[shader_cache][shape_v5][abi3][reflection]") {
    STATIC_REQUIRE(gv::k_gpu_shape_v5_abi_version == 3u);
    STATIC_REQUIRE(gv::k_gpu_shape_v5_partition_required_flags == 7u);
    STATIC_REQUIRE(sizeof(
        gv::GpuShapeV5ResidentPartitionDescriptor) == 104u);
    STATIC_REQUIRE(alignof(
        gv::GpuShapeV5ResidentPartitionDescriptor) == 8u);
    STATIC_REQUIRE(offsetof(
        gv::GpuShapeV5ResidentPartitionDescriptor, hash_byte_offset) == 16u);
    STATIC_REQUIRE(offsetof(
        gv::GpuShapeV5ResidentPartitionDescriptor, control_count) == 48u);
    STATIC_REQUIRE(offsetof(
        gv::GpuShapeV5ResidentPartitionDescriptor, low_hash) == 72u);
    STATIC_REQUIRE(offsetof(
        gv::GpuShapeV5ResidentPartitionDescriptor, segment_end_bp) == 96u);
    STATIC_REQUIRE(sizeof(gv::GpuShapeV5SeedPush) == 72u);
    STATIC_REQUIRE(offsetof(gv::GpuShapeV5SeedPush, status_index) == 68u);
    STATIC_REQUIRE(gv::gpu_shape_v5_seed_status_bit(
        gv::GpuShapeV5SeedStatus::contract) == 1u);
    STATIC_REQUIRE(gv::gpu_shape_v5_seed_status_bit(
        gv::GpuShapeV5SeedStatus::emission) == 16u);
    STATIC_REQUIRE(std::is_standard_layout_v<
        gv::GpuShapeV5ResidentPartitionDescriptor>);
    constexpr gv::GpuShapeV5ResidentPartitionDescriptor descriptor{
        .partition_id = 3u,
        .directory_ordinal = 0u,
        .position_segment = 0u,
        .flags = gv::k_gpu_shape_v5_partition_required_flags,
        .hash_byte_offset = 0u,
        .prefix_byte_offset = 4u,
        .count_byte_offset = 12u,
        .position_byte_offset = 0u,
        .control_count = 2u,
        .position_count = 3u,
        .hash_width = 2u,
        .offset_width = 2u,
        .count_width = 2u,
        .position_width = 2u,
        .low_hash = 7u,
        .high_hash = 11u,
        .segment_base_bp = 100u,
        .segment_end_bp = 200u,
    };
    STATIC_REQUIRE(gv::gpu_shape_v5_partition_descriptor_valid(
        descriptor, 16u, 6u));
    constexpr auto invalid_descriptor = [](auto value) {
        value.position_width = 1u;
        return value;
    }(descriptor);
    STATIC_REQUIRE_FALSE(gv::gpu_shape_v5_partition_descriptor_valid(
        invalid_descriptor, 16u, 6u));

    CHECK(gv::descriptor_binding_count(
        gv::DescriptorLayoutKind::phase_c_shape_v5_seed) == 9u);
    CHECK(gv::descriptor_layout_for(
        gv::ShaderKind::phase_c_shape_v5_seed_count)
        == gv::DescriptorLayoutKind::phase_c_shape_v5_seed);
    CHECK(gv::descriptor_layout_for(
        gv::ShaderKind::phase_c_shape_v5_seed_emit)
        == gv::DescriptorLayoutKind::phase_c_shape_v5_seed);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_shape_v5_seed_count) == 72u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_shape_v5_seed_emit) == 72u);

    const std::array kinds{
        gv::ShaderKind::phase_c_shape_v5_seed_count,
        gv::ShaderKind::phase_c_shape_v5_seed_emit,
    };
    std::vector<gv::ShaderVariantKey> static_keys;
    gv::for_each_static_shader_variant(
        gv::VulkanStaticVariantInputs{},
        [&](gv::ShaderVariantKey key) { static_keys.push_back(key); });
    for (const gv::ShaderKind kind : kinds) {
        const auto key = gv::make_shader_key(kind, 384u, 128u, 3u);
        CHECK(gv::checked_shader_kind(key.kind) == kind);
        CHECK(gv::valid_shader_variant_key(key));
        CHECK_FALSE(gv::valid_shader_variant_key(
            gv::make_shader_key(kind, 384u, 128u, 1u)));
        CHECK(std::count(static_keys.begin(), static_keys.end(), key) == 1);
        const gv::ShaderCompileRequest request{
            .kind = kind,
            .tile_size = 384u,
            .local_size = 128u,
            .max_span = 3u,
        };
        const auto spirv = gv::compile_shader_spirv(request);
        REQUIRE_FALSE(spirv.empty());
        CHECK(spirv_opcode_count(spirv, 241u) > 0u);  // OpAtomicOr status.
        const auto layout = gv::scan_spirv_layout(spirv);
        CHECK(layout.has_binding_array_stride(0u, 1u, 104u));
        CHECK(layout.has_binding_struct_offsets(
            0u, 1u, std::array<std::uint32_t, 18>{
                0u, 4u, 8u, 12u, 16u, 24u, 32u, 40u, 48u,
                52u, 56u, 60u, 64u, 68u, 72u, 80u, 88u, 96u}));
        CHECK(layout.has_push_constant_offsets(
            std::array<std::uint32_t, 18>{
                0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u,
                36u, 40u, 44u, 48u, 52u, 56u, 60u, 64u, 68u}));
    }
}

[[nodiscard]] gv::PersistedShapeV5CoordinateJobAudit
make_shape_v5_coordinate_hit_audit(
    const std::size_t problem_index,
    const std::uint64_t operation_identity)
{
    static gv::Buffer resident_descriptors;
    static gv::Buffer resident_controls;
    static gv::Buffer resident_positions;
    static gv::Buffer query_minimizers;
    const auto identity = [](const gv::Buffer* object,
                             const std::uint64_t token,
                             const std::uint64_t bytes) {
        return gv::RuntimeBufferIdentity{
            .object = object,
            .owner = reinterpret_cast<const gv::Runtime*>(1u),
            .buffer_handle = token,
            .memory_handle = token + 100u,
            .bytes = bytes,
            .allocation_bytes = bytes,
        };
    };
    gv::PersistedShapeV5CoordinateJobAudit audit{};
    audit.operation_identity = operation_identity;
    audit.query_authority_identity = 200u;
    audit.resident_descriptors = identity(
        &resident_descriptors, 1u, 104u);
    audit.resident_controls = identity(&resident_controls, 2u, 64u);
    audit.resident_positions = identity(&resident_positions, 3u, 64u);
    audit.query_minimizer_buffer = identity(&query_minimizers, 4u, 64u);
    audit.device_uuid[0] = 1u;
    audit.descriptor_bytes = 104u;
    audit.control_bytes = 64u;
    audit.position_bytes = 64u;
    audit.position_count = 8u;
    audit.max_occ_cap = 500u;
    audit.partition_count = 1u;
    audit.abi_version = gv::k_gpu_shape_v5_abi_version;
    audit.generation_id = 300u;
    audit.source_identity = 400u;
    audit.cold_load_identity = 500u;
    audit.target_len = 10'000u;
    audit.problem_index = problem_index;
    audit.query_index = static_cast<std::uint32_t>(problem_index);
    audit.logical_target_row = 600u + problem_index;
    audit.logical_query_ordinal = 700u + problem_index;
    audit.query_record = gv::PhaseCSeedQueryRecordWire{
        .minimizer_offset = static_cast<std::uint32_t>(problem_index * 4u),
        .minimizer_count = 4u,
        .query_len = 120u,
        .flags = 0u,
    };
    audit.result = gv::GpuCoordinateOnlyResultWire{
        .query_start = 1u,
        .query_end = 119u,
        .target_start = 100u + problem_index,
        .target_end = 218u + problem_index,
        .match_length = 118u,
        .block_length = 118u,
        .chain_score = 42,
        .seed_count = 7u,
        .is_reverse = 0u,
        .valid = 1u,
    };
    audit.configuration =
        gv::persisted_shape_v5_coordinate_config_identity(
            15u,
            500,
            gv::PhaseCSeedCoordinateAlgorithm::vulkan_tiled_dp,
            gv::ChainingPipelineKey{
                .tile_size = 384u,
                .local_size = 128u,
            },
            5'000,
            5'000,
            500,
            3'000,
            0.01F,
            0.005F,
            25,
            40,
            3,
            16u);
    audit.query_tile_count = 2u;
    audit.query_count_dispatches = 1u;
    audit.query_scan_dispatches = 3u;
    audit.query_emit_dispatches = 1u;
    audit.query_completed_roundtrips = 2u;
    audit.direct_count_dispatches = 1u;
    audit.direct_scan_dispatches = 3u;
    audit.direct_emit_dispatches = 1u;
    audit.anchor_count = 7u;
    audit.sort_operations = 1u;
    audit.range_build_dispatches = 1u;
    audit.range_count = 1u;
    audit.chain_dispatches = 1u;
    audit.chain_backtrack_dispatches = 1u;
    audit.coordinate_dispatches = 1u;
    const std::uint64_t receipt_base = 1'000u + problem_index * 10u;
    audit.count_submit_wait_identity = receipt_base + 1u;
    audit.emit_submit_wait_identity = receipt_base + 2u;
    audit.sort_range_submit_wait_identity = receipt_base + 3u;
    audit.chain_coordinate_submit_wait_identity = receipt_base + 4u;
    audit.count_status_checks = 1u;
    audit.emit_status_checks = 1u;
    audit.result_readbacks = 1u;
    return audit;
}

[[nodiscard]] gv::PersistedShapeV5CoordinateJobExpectation
expectation_for_shape_v5_coordinate_audit(
    const gv::PersistedShapeV5CoordinateJobAudit& audit)
{
    return gv::PersistedShapeV5CoordinateJobExpectation{
        .problem_index = audit.problem_index,
        .query_authority_identity = audit.query_authority_identity,
        .resident_descriptors = audit.resident_descriptors,
        .resident_controls = audit.resident_controls,
        .resident_positions = audit.resident_positions,
        .query_minimizer_buffer = audit.query_minimizer_buffer,
        .device_uuid = audit.device_uuid,
        .descriptor_bytes = audit.descriptor_bytes,
        .control_bytes = audit.control_bytes,
        .position_bytes = audit.position_bytes,
        .position_count = audit.position_count,
        .max_occ_cap = audit.max_occ_cap,
        .partition_count = audit.partition_count,
        .abi_version = audit.abi_version,
        .generation_id = audit.generation_id,
        .source_identity = audit.source_identity,
        .cold_load_identity = audit.cold_load_identity,
        .target_len = audit.target_len,
        .query_index = audit.query_index,
        .logical_target_row = audit.logical_target_row,
        .logical_query_ordinal = audit.logical_query_ordinal,
        .query_record = audit.query_record,
        .result = audit.result,
        .configuration = audit.configuration,
    };
}

TEST_CASE("format-5 seed jobs require strict XOR resident representation",
          "[vulkan][phase_c][shape_v5][route]") {
    gv::Buffer legacy_slots;
    gv::Buffer legacy_positions;
    gv::PhaseCShapeV5ResidentView shape_view{};

    const gv::PhaseCSeedCoordinateJob legacy{
        .slots = &legacy_slots,
        .positions = &legacy_positions,
        .target_len = 100u,
    };
    CHECK(gv::phase_c_seed_index_representation(legacy)
          == gv::PhaseCSeedIndexRepresentation::legacy_slot_table);

    const gv::PhaseCSeedCoordinateJob shape{
        .target_len = 100u,
        .shape_v5 = &shape_view,
    };
    CHECK(gv::phase_c_seed_index_representation(shape)
          == gv::PhaseCSeedIndexRepresentation::shape_v5);

    const gv::PhaseCSeedCoordinateJob ambiguous{
        .slots = &legacy_slots,
        .positions = &legacy_positions,
        .target_len = 100u,
        .shape_v5 = &shape_view,
    };
    CHECK(gv::phase_c_seed_index_representation(ambiguous)
          == gv::PhaseCSeedIndexRepresentation::invalid);

    const gv::PhaseCSeedCoordinateJob shape_with_legacy_metadata{
        .target_len = 100u,
        .position_count = 1u,
        .shape_v5 = &shape_view,
    };
    CHECK(gv::phase_c_seed_index_representation(shape_with_legacy_metadata)
          == gv::PhaseCSeedIndexRepresentation::invalid);

    const gv::PhaseCSeedCoordinateJob incomplete_legacy{
        .slots = &legacy_slots,
        .target_len = 100u,
    };
    CHECK(gv::phase_c_seed_index_representation(incomplete_legacy)
          == gv::PhaseCSeedIndexRepresentation::invalid);
}

TEST_CASE("persisted shape-v5 coordinate proof is strict and one-shot",
          "[vulkan][phase_c][shape_v5][operation-proof]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::DeviceQuerySketchAuthority>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::DeviceQuerySketchAuthority>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::PersistedShapeV5CoordinateOperationProof>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::PersistedShapeV5CoordinateOperationProof>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::PersistedShapeV5CoordinateJobEvidence>);

    auto hit = make_shape_v5_coordinate_hit_audit(0u, 100u);
    REQUIRE(hit.accepted());
    const auto as_zero_minimizer_query = [](auto audit) {
        audit.query_record.minimizer_count = 0u;
        audit.result = {};
        audit.direct_count_dispatches = 0u;
        audit.direct_scan_dispatches = 0u;
        audit.direct_emit_dispatches = 0u;
        audit.anchor_count = 0u;
        audit.sort_operations = 0u;
        audit.range_build_dispatches = 0u;
        audit.range_count = 0u;
        audit.chain_dispatches = 0u;
        audit.chain_backtrack_dispatches = 0u;
        audit.coordinate_dispatches = 0u;
        audit.count_submit_wait_identity = 0u;
        audit.emit_submit_wait_identity = 0u;
        audit.sort_range_submit_wait_identity = 0u;
        audit.chain_coordinate_submit_wait_identity = 0u;
        audit.count_status_checks = 0u;
        audit.emit_status_checks = 0u;
        audit.result_readbacks = 0u;
        return audit;
    };

    SECTION("every mandatory dispatch and completion is required") {
        auto missing = hit;
        missing.query_count_dispatches = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.query_scan_dispatches = 2u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.query_emit_dispatches = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.query_completed_roundtrips = 1u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.direct_count_dispatches = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.direct_scan_dispatches = 2u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.direct_emit_dispatches = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.sort_operations = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.range_build_dispatches = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.chain_dispatches = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.chain_backtrack_dispatches = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.coordinate_dispatches = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.result_readbacks = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.count_status_checks = 0u;
        CHECK_FALSE(missing.accepted());
        missing = hit;
        missing.emit_status_checks = 0u;
        CHECK_FALSE(missing.accepted());
    }

    SECTION("tamper and forbidden-route counters fail closed") {
        auto tampered = hit;
        tampered.status_or = 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.coordinate_vote_dispatches = 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.cpu_or_simd_biological_calls = 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.recompute_calls = 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.fallback_calls = 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.query_record.flags = 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.query_authority_identity = 0u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.resident_controls = {};
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.resident_controls.buffer_handle =
            hit.resident_descriptors.buffer_handle;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.resident_controls.memory_handle =
            hit.resident_descriptors.memory_handle;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.descriptor_bytes =
            hit.resident_descriptors.bytes + 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.partition_count += 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.control_bytes = hit.resident_controls.bytes + 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.position_bytes = hit.resident_positions.bytes + 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.device_uuid = {};
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.configuration.coordinate_algorithm =
            gv::PhaseCSeedCoordinateAlgorithm::coordinate_vote;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.configuration.probe_limit = 0u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.configuration.max_occ =
            static_cast<std::int32_t>(hit.max_occ_cap + 1u);
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.chain_coordinate_submit_wait_identity =
            hit.count_submit_wait_identity;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.result.valid = 2u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.result.is_reverse = 9u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.result.query_end = tampered.query_record.query_len + 1u;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.result.target_start = tampered.result.target_end;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.result.seed_count = -1;
        CHECK_FALSE(tampered.accepted());
        tampered = hit;
        tampered.result.match_length = tampered.result.block_length + 1u;
        CHECK_FALSE(tampered.accepted());
    }

    SECTION("data-dependent no-hit stages remain ineligible") {
        const auto zero_query = as_zero_minimizer_query(hit);
        CHECK_FALSE(zero_query.accepted());
        auto zero_tile_query = zero_query;
        zero_tile_query.query_tile_count = 0u;
        CHECK_FALSE(zero_tile_query.accepted());

        auto no_anchor = hit;
        no_anchor.result = {};
        no_anchor.direct_emit_dispatches = 0u;
        no_anchor.anchor_count = 0u;
        no_anchor.sort_operations = 0u;
        no_anchor.range_build_dispatches = 0u;
        no_anchor.range_count = 0u;
        no_anchor.chain_dispatches = 0u;
        no_anchor.chain_backtrack_dispatches = 0u;
        no_anchor.coordinate_dispatches = 0u;
        no_anchor.emit_submit_wait_identity = 0u;
        no_anchor.sort_range_submit_wait_identity = 0u;
        no_anchor.chain_coordinate_submit_wait_identity = 0u;
        no_anchor.emit_status_checks = 0u;
        no_anchor.result_readbacks = 0u;
        CHECK(no_anchor.accepted());

        auto no_range = hit;
        no_range.result = {};
        no_range.range_count = 0u;
        no_range.chain_dispatches = 0u;
        no_range.chain_backtrack_dispatches = 0u;
        no_range.coordinate_dispatches = 0u;
        no_range.chain_coordinate_submit_wait_identity = 0u;
        no_range.result_readbacks = 0u;
        CHECK(no_range.accepted());
    }

    SECTION("two logical jobs move and consume exactly once") {
        std::array audits{
            make_shape_v5_coordinate_hit_audit(0u, 900u),
            make_shape_v5_coordinate_hit_audit(1u, 900u),
        };
        std::array expectations{
            expectation_for_shape_v5_coordinate_audit(audits[0]),
            expectation_for_shape_v5_coordinate_audit(audits[1]),
        };
        CHECK(gv::PersistedShapeV5CoordinateOperationProof::
              self_test_one_shot_contract(audits, expectations));

        auto unsupported_zero_audits = audits;
        unsupported_zero_audits[1] =
            as_zero_minimizer_query(unsupported_zero_audits[1]);
        std::array unsupported_zero_expectations{
            expectation_for_shape_v5_coordinate_audit(
                unsupported_zero_audits[0]),
            expectation_for_shape_v5_coordinate_audit(
                unsupported_zero_audits[1]),
        };
        CHECK_FALSE(gv::PersistedShapeV5CoordinateOperationProof::
                    self_test_one_shot_contract(
                        unsupported_zero_audits,
                        unsupported_zero_expectations));

        const auto config_tamper_fails = [&](auto mutate) {
            auto tampered = expectations;
            mutate(tampered[0].configuration);
            return !gv::PersistedShapeV5CoordinateOperationProof::
                self_test_one_shot_contract(audits, tampered);
        };
        CHECK(config_tamper_fails([](auto& config) {
            config.kmer_size += 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_occ += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.coordinate_algorithm =
                gv::PhaseCSeedCoordinateAlgorithm::coordinate_vote;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.chain_key.tile_size += 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.chain_key.local_size += 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_dist_x += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_dist_y += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.bandwidth += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_iter += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.gap_penalty_bits ^= 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.skip_penalty_bits ^= 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_chain_skip += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.min_chain_score += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.min_seed_count += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.probe_limit += 1u;
        }));

        expectations[0].result.target_end += 1u;
        CHECK_FALSE(gv::PersistedShapeV5CoordinateOperationProof::
                    self_test_one_shot_contract(audits, expectations));
    }
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

TEST_CASE("SneakySnake rank batch has an exact provider-compatible ABI",
          "[shader_cache][sneaky-snake][reflection][no-fallback]") {
    STATIC_REQUIRE(gv::k_gpu_sneaky_snake_submaze_length == 512u);
    STATIC_REQUIRE(gv::k_gpu_sneaky_snake_max_edits == 31u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(pf::PrefilterStatus::success)
                   == 0u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
                       pf::PrefilterStatus::conservative_unequal) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
                       pf::PrefilterStatus::unsupported_conservative) == 2u);
    STATIC_REQUIRE(sizeof(pf::PrefilterJob) == 20u);
    STATIC_REQUIRE(alignof(pf::PrefilterJob) == 4u);
    STATIC_REQUIRE(offsetof(pf::PrefilterJob, max_edits) == 16u);
    STATIC_REQUIRE(sizeof(pf::PrefilterResult) == 16u);
    STATIC_REQUIRE(alignof(pf::PrefilterResult) == 4u);
    STATIC_REQUIRE(offsetof(pf::PrefilterResult, accepted) == 8u);
    STATIC_REQUIRE(sizeof(gv::GpuPrefilterPush) == 16u);
    STATIC_REQUIRE(offsetof(gv::GpuPrefilterPush, mode) == 4u);
    STATIC_REQUIRE(offsetof(gv::GpuPrefilterPush, query_rank_count) == 8u);
    STATIC_REQUIRE(offsetof(gv::GpuPrefilterPush, target_rank_count) == 12u);

    const auto key = gv::make_shader_key(
        gv::ShaderKind::sneaky_snake_rank_batch, 384u, 64u, 512u);
    REQUIRE(gv::valid_shader_variant_key(key));
    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::sneaky_snake_rank_batch, 384u, 32u, 512u)));
    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::sneaky_snake_rank_batch, 384u, 64u, 511u)));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::ShaderKind::sneaky_snake_rank_batch);
    REQUIRE(gv::descriptor_layout_for(key.to_request().kind)
            == gv::DescriptorLayoutKind::align_extension);
    REQUIRE(gv::descriptor_binding_count(
        gv::descriptor_layout_for(key.to_request().kind)) == 4u);
    REQUIRE(gv::push_constant_size(key.to_request().kind) == 16u);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::ShaderWorkgroupSize{64u, 1u, 1u}));

    const std::string body = gv::shader_body_for_digest(
        gv::ShaderKind::sneaky_snake_rank_batch);
    CHECK(body.find("MAX_SUPPORTED_EDITS = 31u") != std::string::npos);
    CHECK(body.find("uint query_rank_count;") != std::string::npos);
    CHECK(body.find("uint target_rank_count;") != std::string::npos);
    CHECK(body.find("prefilter_job_is_in_bounds(job)") != std::string::npos);
    CHECK(body.find("subgroup") == std::string::npos);
    CHECK(body.find("atomic") == std::string::npos);

    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    const auto scan = gv::scan_spirv_layout(spirv);
    CHECK(scan.has_binding_array_stride(0u, 0u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 1u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 2u, 20u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 2u,
        std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}));
    CHECK(scan.has_binding_array_stride(0u, 3u, 16u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 3u,
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    CHECK(scan.has_push_constant_offsets(
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    REQUIRE(gv::validate_spirv_layout(key, spirv));

    const auto msl = gv::spirv_to_msl(spirv, key, gv::MslVersion{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 64u);
    CHECK(msl->wg_y == 1u);
    CHECK(msl->wg_z == 1u);
}

TEST_CASE("GateKeeper generic and production shaders share one bounds-aware ABI",
          "[shader_cache][gatekeeper][reflection][no-fallback]") {
    STATIC_REQUIRE(gv::k_gpu_gatekeeper_max_length == 128u);
    STATIC_REQUIRE(gv::k_gpu_gatekeeper_max_edits == 8u);
    STATIC_REQUIRE(sizeof(pf::PrefilterJob) == 20u);
    STATIC_REQUIRE(sizeof(pf::PrefilterResult) == 16u);
    STATIC_REQUIRE(sizeof(gv::GpuPrefilterPush) == 16u);

    const auto key = gv::make_shader_key(
        gv::ShaderKind::gatekeeper_rank_batch, 384u, 64u, 128u);
    REQUIRE(gv::valid_shader_variant_key(key));
    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::gatekeeper_rank_batch, 384u, 32u, 128u)));
    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::gatekeeper_rank_batch, 384u, 64u, 127u)));
    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::gatekeeper_rank_batch, 383u, 64u, 128u)));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::ShaderKind::gatekeeper_rank_batch);
    REQUIRE(gv::descriptor_layout_for(key.to_request().kind)
            == gv::DescriptorLayoutKind::align_extension);
    REQUIRE(gv::descriptor_binding_count(
        gv::descriptor_layout_for(key.to_request().kind)) == 4u);
    REQUIRE(gv::push_constant_size(key.to_request().kind) == 16u);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::ShaderWorkgroupSize{64u, 1u, 1u}));

    struct ProductionRouteExpectation {
        gv::ShaderKind kind;
        std::uint32_t length;
        std::uint32_t max_edits;
        std::size_t replay_index;
    };
    const std::array production_routes{
        ProductionRouteExpectation{
            gv::ShaderKind::gatekeeper_rank_batch_l32_e2, 32u, 2u, 3u},
        ProductionRouteExpectation{
            gv::ShaderKind::gatekeeper_rank_batch_l32_e4, 32u, 4u, 4u},
        ProductionRouteExpectation{
            gv::ShaderKind::gatekeeper_rank_batch_l64_e2, 64u, 2u, 5u},
        ProductionRouteExpectation{
            gv::ShaderKind::gatekeeper_rank_batch_l64_e4, 64u, 4u, 6u},
    };

    std::array<bool, gv::k_rank_prefilter_replay_route_count>
        observed_replay_indices{};
    const auto check_replay_route = [
        &observed_replay_indices](
            gv::ShaderKind kind,
            std::uint32_t push_mode,
            std::size_t expected_index) {
        const std::optional<std::size_t> replay_index =
            gv::rank_prefilter_replay_index(kind, push_mode);
        REQUIRE(replay_index);
        REQUIRE(*replay_index < observed_replay_indices.size());
        CHECK(*replay_index == expected_index);
        CHECK_FALSE(observed_replay_indices[*replay_index]);
        observed_replay_indices[*replay_index] = true;
    };
    check_replay_route(
        gv::ShaderKind::sneaky_snake_rank_batch, 1u, 0u);
    check_replay_route(
        gv::ShaderKind::sneaky_snake_rank_batch, 8u, 1u);
    check_replay_route(
        gv::ShaderKind::gatekeeper_rank_batch, 0u, 2u);

    const gv::GatekeeperRankBatchRoute empty_route =
        gv::resolve_gatekeeper_rank_batch_route(
            std::span<const pf::PrefilterJob>{});
    CHECK(empty_route.kind == gv::ShaderKind::gatekeeper_rank_batch);
    CHECK(empty_route.replay_index == 2u);

    std::vector<std::string> rank_prefilter_cache_stems{
        gv::cache_file_stem(key)};
    for (const ProductionRouteExpectation& production_route
         : production_routes) {
        const gv::ShaderKind production_kind = production_route.kind;
        const auto production_key = gv::make_shader_key(
            production_kind, 384u, 64u, 128u);
        const std::string production_cache_stem =
            gv::cache_file_stem(production_key);
        CAPTURE(production_cache_stem);
        CHECK(std::find(
            rank_prefilter_cache_stems.begin(),
            rank_prefilter_cache_stems.end(),
            production_cache_stem) == rank_prefilter_cache_stems.end());
        rank_prefilter_cache_stems.push_back(production_cache_stem);
        REQUIRE(gv::valid_shader_variant_key(production_key));
        REQUIRE(gv::descriptor_layout_for(production_kind)
                == gv::DescriptorLayoutKind::align_extension);
        REQUIRE(gv::push_constant_size(production_kind) == 16u);
        check_replay_route(
            production_kind, 0u, production_route.replay_index);

        const std::array production_jobs{
            pf::PrefilterJob{
                .query_offset = 0u,
                .target_offset = 0u,
                .query_length = production_route.length,
                .target_length = production_route.length,
                .max_edits = production_route.max_edits,
            },
            pf::PrefilterJob{
                .query_offset = production_route.length,
                .target_offset = production_route.length,
                .query_length = production_route.length,
                .target_length = production_route.length,
                .max_edits = production_route.max_edits,
            },
        };
        const gv::GatekeeperRankBatchRoute resolved_route =
            gv::resolve_gatekeeper_rank_batch_route(production_jobs);
        CHECK(resolved_route.kind == production_kind);
        CHECK(resolved_route.replay_index == production_route.replay_index);

        const std::string production_body =
            gv::shader_body_for_digest(production_kind);
        CHECK(production_body.find(
            "#define GATEKEEPER_SPECIALIZED_LENGTH "
            + std::to_string(production_route.length))
              != std::string::npos);
        CHECK(production_body.find(
            "#define GATEKEEPER_SPECIALIZED_EDITS "
            + std::to_string(production_route.max_edits))
              != std::string::npos);

        const auto production_spirv =
            gv::compile_shader_spirv(production_key.to_request());
        REQUIRE_FALSE(production_spirv.empty());
        const auto production_scan =
            gv::scan_spirv_layout(production_spirv);
        CHECK(production_scan.has_binding_array_stride(0u, 0u, 4u));
        CHECK(production_scan.has_binding_array_stride(0u, 1u, 4u));
        CHECK(production_scan.has_binding_array_stride(0u, 2u, 20u));
        CHECK(production_scan.has_binding_struct_offsets(
            0u, 2u,
            std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}));
        CHECK(production_scan.has_binding_array_stride(0u, 3u, 16u));
        CHECK(production_scan.has_binding_struct_offsets(
            0u, 3u,
            std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
        CHECK(production_scan.has_push_constant_offsets(
            std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
        REQUIRE(gv::validate_spirv_layout(
            production_key, production_spirv));

        const auto production_msl = gv::spirv_to_msl(
            production_spirv, production_key, gv::MslVersion{});
        REQUIRE(production_msl);
        CHECK(production_msl->wg_x == 64u);
        CHECK(production_msl->wg_y == 1u);
        CHECK(production_msl->wg_z == 1u);
    }
    CHECK(std::all_of(
        observed_replay_indices.begin(),
        observed_replay_indices.end(),
        [](bool observed) { return observed; }));
    CHECK_FALSE(gv::rank_prefilter_replay_index(
        gv::ShaderKind::sneaky_snake_rank_batch, 0u));
    CHECK_FALSE(gv::rank_prefilter_replay_index(
        gv::ShaderKind::gatekeeper_rank_batch, 1u));

    const std::array mixed_production_jobs{
        pf::PrefilterJob{
            .query_offset = 0u,
            .target_offset = 0u,
            .query_length = 32u,
            .target_length = 32u,
            .max_edits = 2u,
        },
        pf::PrefilterJob{
            .query_offset = 32u,
            .target_offset = 32u,
            .query_length = 64u,
            .target_length = 64u,
            .max_edits = 2u,
        },
    };
    const gv::GatekeeperRankBatchRoute mixed_route =
        gv::resolve_gatekeeper_rank_batch_route(mixed_production_jobs);
    CHECK(mixed_route.kind == gv::ShaderKind::gatekeeper_rank_batch);
    CHECK(mixed_route.replay_index == 2u);

    const std::array unaligned_production_job{
        pf::PrefilterJob{
            .query_offset = 1u,
            .target_offset = 0u,
            .query_length = 32u,
            .target_length = 32u,
            .max_edits = 2u,
        },
    };
    const gv::GatekeeperRankBatchRoute unaligned_route =
        gv::resolve_gatekeeper_rank_batch_route(unaligned_production_job);
    CHECK(unaligned_route.kind == gv::ShaderKind::gatekeeper_rank_batch);
    CHECK(unaligned_route.replay_index == 2u);

    const std::string body = gv::shader_body_for_digest(
        gv::ShaderKind::gatekeeper_rank_batch);
    CHECK(body.find("MAX_GATEKEEPER_LENGTH = 128u")
          != std::string::npos);
    CHECK(body.find("MAX_GATEKEEPER_EDITS = 8u")
          != std::string::npos);
    CHECK(body.find("prefilter_job_is_in_bounds(job)")
          != std::string::npos);
    CHECK(body.find("set_mask_bit(shifted_mask, shift - 1u)")
          != std::string::npos);
    CHECK(body.find("set_mask_bit(shifted_mask, length - shift)")
          != std::string::npos);
    CHECK(body.find("exact_edit_distance_is_above_threshold(job)")
          != std::string::npos);
    CHECK(body.find("HAMMING_EXACT_REJECTION_MASK")
          != std::string::npos);
    CHECK(body.find("subgroup") == std::string::npos);
    CHECK(body.find("atomic") == std::string::npos);

    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    const auto scan = gv::scan_spirv_layout(spirv);
    CHECK(scan.has_binding_array_stride(0u, 0u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 1u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 2u, 20u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 2u,
        std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}));
    CHECK(scan.has_binding_array_stride(0u, 3u, 16u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 3u,
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    CHECK(scan.has_push_constant_offsets(
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    REQUIRE(gv::validate_spirv_layout(key, spirv));

    const auto msl = gv::spirv_to_msl(spirv, key, gv::MslVersion{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 64u);
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

TEST_CASE("sealed shader cache manifest fixes exact AOT membership and bytes",
          "[shader_cache][sealed-manifest][immutable-cache]") {
    const auto abc = bytes_of("abc");
    constexpr std::array<std::uint8_t, 32> abc_sha256{
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    CHECK(gv::sealed_shader_cache_detail::sha256(abc) == abc_sha256);

    TempDir temp;
    constexpr std::string_view digest = "0123456789abcdef";
    const gv::ShaderVariantKey key_64 = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    const gv::ShaderVariantKey key_32 = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 32u);
    const std::array requested{key_64, key_32};
    write_fake_sealed_shader_artifacts(temp.path, requested);

    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, requested);
    REQUIRE(sealed);
    REQUIRE(sealed->keys.size() == requested.size());
    CHECK(std::none_of(
        sealed->artifacts.begin(),
        sealed->artifacts.end(),
        [](const gv::SealedShaderCacheArtifact& artifact) {
            return artifact.kind
                == gv::SealedShaderCacheArtifactKind::reserved_command_metallib;
        }));
    CHECK(sealed->keys[0] == key_32);
    CHECK(sealed->keys[1] == key_64);
    CHECK(sealed->seed_size > 0u);
    CHECK(sealed->manifest_checksum != 0u);
    CHECK(std::filesystem::file_size(
              gv::sealed_shader_cache_manifest_path(temp.path)) > 0u);

    // Expected membership is a set at the API, but the stored wire image is
    // canonical and exact. Reordering the request cannot change acceptance.
    const std::array reverse_expected{key_64, key_32};
    const auto loaded = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, reverse_expected);
    REQUIRE(loaded);
    CHECK(loaded->seed_sha256 == sealed->seed_sha256);
    CHECK(loaded->artifacts == sealed->artifacts);

    shader_module_create_called = false;
    const auto module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE,
        temp.path,
        digest,
        reverse_expected,
        key_64,
        &record_shader_module_create);
    REQUIRE(module);
    CHECK(shader_module_create_called);

    shader_module_create_called = false;
    const gv::ShaderVariantKey nonmember = gv::make_shader_key(
        gv::ShaderKind::align_extend_banded_affine, 384u, 1u, 512u);
    const auto rejected = gv::load_sealed_cached_module(
        VK_NULL_HANDLE,
        temp.path,
        digest,
        reverse_expected,
        nonmember,
        &record_shader_module_create);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("not a member of the sealed AOT seed")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    const std::array wrong_expected{key_64};
    const auto wrong_membership =
        gv::load_validated_sealed_shader_cache_manifest(
            temp.path, digest, wrong_expected);
    REQUIRE_FALSE(wrong_membership);
    CHECK(wrong_membership.error().find("key membership mismatch")
          != std::string::npos);
}

TEST_CASE("sealed shader module uses the exact already-hashed metallib image",
          "[shader_cache][sealed-manifest][immutable-cache][toctou]") {
    TempDir temp;
    constexpr std::string_view digest = "1122334455667788";
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));

    auto image = gv::load_sealed_shader_cache_key_image(
        temp.path, digest, keys, key);
    REQUIRE(image);
    expected_shader_module_metallib = image->metallib_bytes;
    const auto replacement = bytes_of("mutated-after-verified-image-load");
    REQUIRE(gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key), replacement));

    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto module = gv::create_shader_module_from_sealed_key_image(
        VK_NULL_HANDLE, *image, &record_exact_shader_module_create);
    REQUIRE(module);
    CHECK(shader_module_create_called);
    CHECK(shader_module_payload_matches);

    // A fresh provider call sees the mutation and fails before Vulkan. The
    // previously verified image remains immutable and needs no pathname reopen.
    shader_module_create_called = false;
    const auto rejected = gv::load_sealed_cached_module(
        VK_NULL_HANDLE,
        temp.path,
        digest,
        keys,
        key,
        &record_shader_module_create);
    REQUIRE_FALSE(rejected);
    CHECK_FALSE(shader_module_create_called);
    CHECK((rejected.error().find("artifact size mismatch") != std::string::npos
           || rejected.error().find("artifact SHA-256 mismatch")
               != std::string::npos));
    expected_shader_module_metallib.clear();
}

TEST_CASE("process seed reads once and survives source mutation and deletion",
          "[shader_cache][sealed-manifest][seed-image][immutable-cache][toctou]") {
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
                         gv::SealedShaderCacheSeedReceipt>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
                         gv::SealedShaderCacheSeedImage>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<
                         gv::SealedShaderCacheSeedImage>);

    TempDir temp;
    constexpr std::string_view digest = "22446688aaccee00";
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    const gv::ShaderVariantKey earlier_key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 32u);
    // This intentionally matches producer traversal rather than manifest
    // ordering; every production consumer uses the shared canonical contract.
    const std::array keys{key, earlier_key};
    const auto canonical = gv::canonical_sealed_shader_cache_keys(keys);
    REQUIRE(canonical);
    REQUIRE(canonical->front() == earlier_key);
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));

    gv::SealedShaderCacheProcessStore process_store;
    auto first = process_store.load_once(temp.path, digest, keys);
    REQUIRE(first);
    const std::array reverse_keys{earlier_key, key};
    auto second = process_store.load_once(temp.path, digest, reverse_keys);
    REQUIRE(second);
    CHECK(first->get() == second->get());
    CHECK((*first)->receipt().identity() == (*second)->receipt().identity());
    CHECK(process_store.filesystem_load_count() == 1u);
    const auto seed = *first;
    REQUIRE(seed->accepted());
    const auto& receipt = seed->receipt();
    CHECK(receipt.load_count() == 1u);
    CHECK(receipt.manifest_read_count() == 1u);
    CHECK(receipt.artifact_read_count() == seed->artifacts().size());
    CHECK(receipt.artifact_bytes() == seed->manifest().seed_size);
    CHECK(receipt.manifest_bytes() == seed->manifest_bytes().size());
    CHECK(receipt.load_ns() > 0u);
    CHECK(receipt.admission_count() == 1u);
    CHECK(receipt.peak_admitted_bytes() >=
          receipt.retained_capacity_bytes());
    CHECK(receipt.retained_capacity_bytes()
          == seed->owned_capacity_bytes());
    CHECK_FALSE(receipt.admission_saturated());

    auto key_view = gv::sealed_shader_cache_key_view(*seed, key);
    REQUIRE(key_view);
    expected_shader_module_metallib.assign(
        key_view->metallib_bytes.begin(), key_view->metallib_bytes.end());

    const auto archive = std::find_if(
        seed->artifacts().begin(), seed->artifacts().end(),
        [](const gv::SealedShaderCacheArtifactImage& image) {
            return image.descriptor.kind
                == gv::SealedShaderCacheArtifactKind::metal_binary_archive;
        });
    REQUIRE(archive != seed->artifacts().end());
    const auto qualification = std::find_if(
        seed->artifacts().begin(), seed->artifacts().end(),
        [&archive](const gv::SealedShaderCacheArtifactImage& image) {
            return image.descriptor.kind
                    == gv::SealedShaderCacheArtifactKind::
                        metal_binary_archive_qualification
                && image.descriptor.filename
                    == archive->descriptor.filename + ".qualification";
        });
    REQUIRE(qualification != seed->artifacts().end());
    REQUIRE(gv::sealed_shader_cache_detail::
        parse_metal_archive_qualification_receipt(qualification->bytes));

    REQUIRE(gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key), bytes_of("mutated")));
    std::error_code remove_error;
    std::filesystem::remove_all(temp.path, remove_error);
    REQUIRE_FALSE(remove_error);

    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE, *seed, key, &record_exact_shader_module_create);
    REQUIRE(module);
    CHECK(shader_module_create_called);
    CHECK(shader_module_payload_matches);

    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    admission.set_index_memory_cap_bytes(admission.reserved_bytes());
    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto denied_module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE, *seed, key, &record_exact_shader_module_create);
    admission.set_index_memory_cap_bytes(previous_cap);
    REQUIRE_FALSE(denied_module);
    CHECK(denied_module.error().find("module admission denied")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    shader_module_create_called = false;
    shader_module_payload_matches = false;
    const auto second_module = gv::load_sealed_cached_module(
        VK_NULL_HANDLE, *seed, key, &record_exact_shader_module_create);
    REQUIRE(second_module);
    CHECK(shader_module_create_called);
    CHECK(shader_module_payload_matches);
    CHECK(gv::sealed_shader_cache_detail::sha256(archive->bytes)
          == archive->descriptor.sha256);
    expected_shader_module_metallib.clear();
}

TEST_CASE("process seed rejects oversized manifest before artifact allocation",
          "[shader_cache][sealed-manifest][seed-image][admission][corruption]") {
    TempDir temp;
    constexpr std::string_view digest = "33557799bbddff11";
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE(sealed);

    gv::SealedShaderCacheManifest oversized = *sealed;
    oversized.seed_size =
        gv::sealed_shader_cache_detail::maximum_seed_image_bytes + 1u;
    const auto wire =
        gv::sealed_shader_cache_detail::serialize_manifest(oversized);
    REQUIRE(wire);
    REQUIRE(gv::durable_write_file(
        gv::sealed_shader_cache_manifest_path(temp.path), wire->first));
    const auto rejected = gv::load_validated_sealed_shader_cache_seed(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("exceeds bounded ownership")
          != std::string::npos);
}

TEST_CASE("seal rejects oversized seed before publishing or reading it",
          "[shader_cache][sealed-manifest][seed-image][admission][publication]") {
    TempDir temp;
    constexpr std::string_view digest = "22446688aaccee00";
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    std::error_code error;
    std::filesystem::resize_file(
        gv::cache_metallib_path(temp.path, key),
        gv::sealed_shader_cache_detail::maximum_seed_image_bytes + 1u,
        error);
    REQUIRE_FALSE(error);
    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(sealed);
    CHECK(sealed.error().find("beyond byte cap") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path)));
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path).string() + ".tmp"));
}

TEST_CASE("seal validates the admitted candidate before atomic publication",
          "[shader_cache][sealed-manifest][seed-image][publication][corruption]") {
    TempDir temp;
    constexpr std::string_view digest = "1133557799bbddff";
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::durable_write_file(
        gv::cache_meta_path(temp.path, key), bytes_of("bad-meta")));
    const auto sealed = gv::seal_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(sealed);
    CHECK(sealed.error().find("metadata incompatible") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path)));
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path).string() + ".tmp"));
}

TEST_CASE("seal rejects same-size mutation between read and publication",
          "[shader_cache][sealed-manifest][publication][toctou][corruption]") {
    TempDir temp;
    constexpr std::string_view digest = "0011223344556677";
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    seal_identity_mutation_path = gv::cache_metallib_path(temp.path, key);
    seal_identity_mutation_succeeded = false;
    const auto sealed = gv::seal_shader_cache_manifest_for_test(
        temp.path, digest, keys, &mutate_seal_artifact_same_size);
    seal_identity_mutation_path.clear();
    REQUIRE(seal_identity_mutation_succeeded);
    REQUIRE_FALSE(sealed);
    CHECK(sealed.error().find("identity changed before publication")
          != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path)));
    CHECK_FALSE(std::filesystem::exists(
        gv::sealed_shader_cache_manifest_path(temp.path).string() + ".tmp"));
}

TEST_CASE("private archive staging canonicalizes a symlinked temp parent",
          "[shader_cache][seed-image][metal-archive][private-snapshot]") {
#if !defined(__APPLE__)
    SKIP("private Metal archive snapshots are Apple-only");
#else
    TempDir temp;
    const std::filesystem::path real_parent = temp.path / "real";
    const std::filesystem::path linked_parent = temp.path / "linked";
    std::error_code error;
    std::filesystem::create_directory(real_parent, error);
    REQUIRE_FALSE(error);
    std::filesystem::create_directory_symlink(
        real_parent, linked_parent, error);
    REQUIRE_FALSE(error);

    const auto retained = bytes_of("archive-source-is-already-deleted");
    const std::filesystem::path source = temp.path / "source.metalar";
    REQUIRE(gv::durable_write_file(source, retained));
    std::filesystem::remove(source, error);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(std::filesystem::exists(source));

    const auto snapshot = gv::verify_runtime_private_archive_snapshot_for_test(
        "safe-test.metalar", retained, linked_parent);
    REQUIRE(snapshot);
    CHECK(snapshot->snapshot_path.is_absolute());
    CHECK(snapshot->snapshot_path.parent_path()
          == std::filesystem::canonical(real_parent));
    CHECK(snapshot->streamed_bytes == retained.size());
    CHECK(snapshot->scratch_capacity_bytes == 64u * 1024u);
    CHECK(snapshot->exact_seed_compared);
    CHECK(snapshot->tamper_rejected);
    CHECK_FALSE(std::filesystem::exists(snapshot->snapshot_path));
#endif
}

TEST_CASE("sealed shader cache rejects artifact, wire, and symlink mutation",
          "[shader_cache][sealed-manifest][immutable-cache][corruption]") {
    TempDir temp;
    constexpr std::string_view digest = "fedcba9876543210";
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));

    const std::filesystem::path metallib =
        gv::cache_metallib_path(temp.path, key);
    const std::vector<std::byte> original_metallib = read_file_bytes(metallib);
    const auto corrupted = bytes_of("same-cache-name-different-metallib");
    REQUIRE(gv::durable_write_file(metallib, corrupted));
    auto rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK((rejected.error().find("artifact size mismatch") != std::string::npos
           || rejected.error().find("artifact SHA-256 mismatch")
               != std::string::npos));
    REQUIRE(gv::durable_write_file(metallib, original_metallib));
    REQUIRE(gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys));

    const std::filesystem::path qualification =
        gv::cache_binary_archive_qualification_path(
            gv::cache_binary_archive_path(temp.path, key));
    const std::vector<std::byte> original_qualification =
        read_file_bytes(qualification);
    std::vector<std::byte> corrupt_qualification = original_qualification;
    REQUIRE(corrupt_qualification.size() > 50u);
    corrupt_qualification[50u] ^= std::byte{0x01};
    REQUIRE(gv::durable_write_file(qualification, corrupt_qualification));
    rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("artifact SHA-256 mismatch")
          != std::string::npos);
    REQUIRE(gv::durable_write_file(
        qualification, original_qualification));
    REQUIRE(gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys));

    const std::filesystem::path manifest_path =
        gv::sealed_shader_cache_manifest_path(temp.path);
    const std::vector<std::byte> original_manifest =
        read_file_bytes(manifest_path);
    std::vector<std::byte> corrupt_manifest = original_manifest;
    REQUIRE(corrupt_manifest.size() > 8u);
    corrupt_manifest.back() ^= std::byte{0x01};
    REQUIRE(gv::durable_write_file(manifest_path, corrupt_manifest));
    rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("checksum mismatch") != std::string::npos);
    REQUIRE(gv::durable_write_file(manifest_path, original_manifest));

    const std::filesystem::path owned_metallib = metallib.string() + ".owned";
    std::error_code ec;
    std::filesystem::rename(metallib, owned_metallib, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::create_symlink(
        owned_metallib.filename(), metallib, ec);
    REQUIRE_FALSE(ec);
    rejected = gv::load_validated_sealed_shader_cache_manifest(
        temp.path, digest, keys);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().find("must not be a symlink") != std::string::npos);
}

TEST_CASE("archive helper success sentinel is unique and final",
          "[shader_cache][metal-archive][immutable-cache]") {
    CHECK(gv::archive_helper_transcript_is_exact_success(
        "qualification evidence\nRESULT: ok\n"));
    CHECK_FALSE(gv::archive_helper_transcript_is_exact_success(
        "qualification evidence\nRESULT: failed\n"));
    CHECK_FALSE(gv::archive_helper_transcript_is_exact_success(
        "RESULT: ok\ntrailing output\n"));
    CHECK_FALSE(gv::archive_helper_transcript_is_exact_success(
        "RESULT: failed\nRESULT: ok\n"));
    CHECK_FALSE(gv::archive_helper_transcript_is_exact_success("RESULT: ok"));
}

TEST_CASE("archive miss NSError fingerprint and receipt are canonical",
          "[shader_cache][metal-archive][immutable-cache][qualification]") {
    constexpr std::string_view transcript =
        "same-descriptor empty-archive negative control: ok\n"
        "NEGATIVE_CONTROL_NSError domain_hex="
        "4d544c4c6962726172794572726f72446f6d61696e code=-7\n"
        "RESULT: ok\n";
    const auto fingerprint =
        gv::parse_archive_helper_negative_error_fingerprint(transcript);
    REQUIRE(fingerprint);
    CHECK(fingerprint->domain == "MTLLibraryErrorDomain");
    CHECK(fingerprint->code == -7);

    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        "NEGATIVE_CONTROL_NSError domain_hex=4D54 code=1\nRESULT: ok\n"));
    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        "NEGATIVE_CONTROL_NSError domain_hex=4d54 code=nope\nRESULT: ok\n"));
    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        "prefix NEGATIVE_CONTROL_NSError domain_hex=4d54 code=1\n"
        "RESULT: ok\n"));
    CHECK_FALSE(gv::parse_archive_helper_negative_error_fingerprint(
        std::string{transcript} + std::string{transcript}));

    const auto helper_qualification =
        gv::parse_archive_helper_qualification_v2(
            "HELPER_QUALIFICATION_V2 protocol=2 "
            "build=bioseq_gpu_archive_builder_v2 registry_id=4660\n"
            "RESULT: ok\n");
    REQUIRE(helper_qualification);
    CHECK(helper_qualification->protocol_version == 2u);
    CHECK(helper_qualification->build_identity
          == "bioseq_gpu_archive_builder_v2");
    CHECK(helper_qualification->metal_registry_id == 4660u);
    CHECK_FALSE(gv::parse_archive_helper_qualification_v2(
        "HELPER_QUALIFICATION_V2 protocol=1 "
        "build=bioseq_gpu_archive_builder_v2 registry_id=4660\n"));
    CHECK_FALSE(gv::parse_archive_helper_qualification_v2(
        "HELPER_QUALIFICATION_V2 protocol=2 build=other registry_id=4660\n"));
    CHECK_FALSE(gv::parse_archive_helper_qualification_v2(
        "HELPER_QUALIFICATION_V2 protocol=2 "
        "build=bioseq_gpu_archive_builder_v2 registry_id=0\n"));

    const auto archive = bytes_of("qualified-archive-byte-image");
    const auto metallib = bytes_of("qualified-metallib-byte-image");
    const auto helper = bytes_of("qualified-helper-byte-image");
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    const gv::MetalArchiveQualificationReceipt receipt{
        .wire_version =
            gv::sealed_shader_cache_detail::archive_qualification_version,
        .shader_key = key,
        .metallib_size = static_cast<std::uint64_t>(metallib.size()),
        .metallib_sha256 =
            gv::sealed_shader_cache_detail::sha256(metallib),
        .entry_name = "main0",
        .max_total_threads_per_threadgroup = 256u,
        .threadgroup_size_is_multiple = false,
        .negative_error = *fingerprint,
        .archive_size = static_cast<std::uint64_t>(archive.size()),
        .archive_sha256 = gv::sealed_shader_cache_detail::sha256(archive),
        .helper_protocol_version = 2u,
        .helper_build_identity = "bioseq_gpu_archive_builder_v2",
        .helper_source = "installed-sibling",
        .helper_executable_size = static_cast<std::uint64_t>(helper.size()),
        .helper_executable_sha256 =
            gv::sealed_shader_cache_detail::sha256(helper),
        .helper_metal_registry_id = 0x1234u,
    };
    auto wire = gv::sealed_shader_cache_detail::
        serialize_metal_archive_qualification_receipt(receipt);
    REQUIRE(wire);
    const auto parsed = gv::sealed_shader_cache_detail::
        parse_metal_archive_qualification_receipt(*wire);
    REQUIRE(parsed);
    CHECK(*parsed == receipt);

    REQUIRE(wire->size() > 50u);
    wire->at(50u) ^= std::byte{0x01};
    const auto corrupted = gv::sealed_shader_cache_detail::
        parse_metal_archive_qualification_receipt(*wire);
    REQUIRE_FALSE(corrupted);
    CHECK(corrupted.error().find("checksum mismatch") != std::string::npos);
}

TEST_CASE("failed archive helper output is never promoted",
          "[shader_cache][metal-archive][immutable-cache]") {
#if !defined(__APPLE__)
    SKIP("Metal archive helper is an Apple-only publication path");
#else
    TempDir temp;
    const std::filesystem::path helper = temp.path / "false_archive_helper.sh";
    const auto script = bytes_of(
        "#!/bin/sh\n"
        "printf 'partial archive bytes' > \"$2\"\n"
        "printf 'qualification failed without success sentinel\\n'\n"
        "exit 0\n");
    REQUIRE(gv::durable_write_file(helper, script));
    REQUIRE(::chmod(helper.c_str(), 0700) == 0);
    ScopedEnvironmentVariable test_only_env{
        "BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY", "1"};
    ScopedEnvironmentVariable helper_env{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};
    ScopedArchiveHelperStoreReset helper_store_reset;

    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    const std::array keys{key};
    write_fake_sealed_shader_artifacts(temp.path, keys);
    constexpr std::string_view forged_entry = "wrong_semantic_entry";
    const gv::ShaderWorkgroupSize workgroup =
        gv::expected_shader_workgroup_size(key);
    gv::ShaderCacheMeta forged_meta{};
    forged_meta.key = key;
    forged_meta.wg_x = workgroup.x;
    forged_meta.wg_y = workgroup.y;
    forged_meta.wg_z = workgroup.z;
    forged_meta.entry_len = static_cast<std::uint32_t>(forged_entry.size());
    REQUIRE(gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        gv::meta_to_bytes(forged_meta, forged_entry)));
    REQUIRE(gv::cache_entry_complete(temp.path, key));
    const std::vector<std::byte> stale_meta = read_file_bytes(
        gv::cache_meta_path(temp.path, key));
    const std::vector<std::byte> stale_metallib = read_file_bytes(
        gv::cache_metallib_path(temp.path, key));
    const std::filesystem::path final =
        gv::cache_binary_archive_path(temp.path, key);
    REQUIRE(std::filesystem::exists(final));

    // A merely nonempty preexisting archive is not reusable while building a
    // new seed. Qualification removes it first; failure leaves nothing that a
    // later manifest publication could accidentally bless.
    const auto result = gv::ensure_compiled_for_sealed_seed(
        temp.path, key, gv::MslVersion{});
    REQUIRE_FALSE(result);
    CHECK(result.error().find("without exact success sentinel")
          != std::string::npos);
    CHECK(read_file_bytes(gv::cache_meta_path(temp.path, key)) != stale_meta);
    CHECK(read_file_bytes(gv::cache_metallib_path(temp.path, key))
          != stale_metallib);
    CHECK_FALSE(std::filesystem::exists(final));
    CHECK_FALSE(std::filesystem::exists(final.string() + ".tmp"));
    const auto cannot_seal = gv::seal_shader_cache_manifest(
        temp.path, "0123456789abcdef", keys);
    REQUIRE_FALSE(cannot_seal);
    CHECK(cannot_seal.error().find(
              "cannot seal non-regular shader artifact")
          != std::string::npos);
    CHECK(cannot_seal.error().find(final.string()) != std::string::npos);
#endif
}

TEST_CASE("legacy archive qualification is diagnostic only",
          "[shader_cache][metal-archive][qualification][wire-v1]") {
    const auto archive = bytes_of("legacy-qualified-archive");
    std::vector<std::byte> wire;
    gv::sealed_shader_cache_detail::append_text(
        wire, gv::sealed_shader_cache_detail::archive_qualification_magic);
    gv::sealed_shader_cache_detail::append_u32(
        wire,
        gv::sealed_shader_cache_detail::archive_qualification_legacy_version);
    gv::sealed_shader_cache_detail::append_u32(wire, 0u);
    gv::sealed_shader_cache_detail::append_u64(wire, 256u);
    gv::sealed_shader_cache_detail::append_u32(wire, 0u);
    constexpr std::string_view domain = "MTLLibraryErrorDomain";
    gv::sealed_shader_cache_detail::append_u32(
        wire, static_cast<std::uint32_t>(domain.size()));
    gv::sealed_shader_cache_detail::append_u64(
        wire, std::bit_cast<std::uint64_t>(std::int64_t{-7}));
    gv::sealed_shader_cache_detail::append_u64(
        wire, static_cast<std::uint64_t>(archive.size()));
    gv::sealed_shader_cache_detail::append_digest(
        wire, gv::sealed_shader_cache_detail::sha256(archive));
    gv::sealed_shader_cache_detail::append_text(wire, domain);
    gv::sealed_shader_cache_detail::append_u64(
        wire, gv::sealed_shader_cache_detail::manifest_checksum(wire));

    const auto parsed = gv::sealed_shader_cache_detail::
        parse_metal_archive_qualification_receipt(wire);
    REQUIRE(parsed);
    CHECK(parsed->wire_version
          == gv::sealed_shader_cache_detail::
              archive_qualification_legacy_version);
    CHECK(parsed->diagnostic_valid());
    CHECK_FALSE(parsed->accepted());
    CHECK_FALSE(parsed->production_accepted());
}

TEST_CASE("archive helper qualification is terminal and hashes once",
          "[shader_cache][metal-archive][qualification][helper]") {
#if !defined(__APPLE__)
    SKIP("archive helper qualification is Apple-only");
#else
    TempDir temp;
    const std::filesystem::path helper = temp.path / "helper.sh";
    const auto helper_bytes = bytes_of("#!/bin/sh\nexit 1\n");
    REQUIRE(gv::durable_write_file(helper, helper_bytes));
    REQUIRE(::chmod(helper.c_str(), 0700) == 0);
    ScopedEnvironmentVariable test_only{
        "BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY", "1"};
    ScopedEnvironmentVariable override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};

    gv::QualifiedMetalArchiveHelperProcessStore store;
    const auto first = store.load_once();
    REQUIRE(first);
    CHECK((**first).source == "test-env-override");
    CHECK(store.qualification_attempt_count() == 1u);
    CHECK(store.filesystem_hash_count() == 1u);
    REQUIRE(gv::durable_write_file(helper, bytes_of("mutated")));
    const auto second = store.load_once();
    REQUIRE(second);
    CHECK(first->get() == second->get());
    CHECK((**second).executable_bytes == helper_bytes);
    CHECK(store.qualification_attempt_count() == 1u);
    CHECK(store.filesystem_hash_count() == 1u);

    const std::filesystem::path metallib = temp.path / "input.metallib";
    REQUIRE(gv::durable_write_file(metallib, bytes_of("metallib")));
    const int metallib_fd = ::open(
        metallib.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(metallib_fd >= 0);
    auto staged = gv::stage_archive_helper_invocation(**first, metallib_fd);
    REQUIRE(staged);
    const std::filesystem::path staged_path = staged->helper_path();
    const std::filesystem::path staged_directory = staged_path.parent_path();
    CHECK(staged->helper_identity_matches());
    CHECK(std::filesystem::exists(staged_path));
    REQUIRE(staged->finish());
    CHECK_FALSE(std::filesystem::exists(staged_path));
    CHECK_FALSE(std::filesystem::exists(staged_directory));
    REQUIRE(::close(metallib_fd) == 0);

    const std::filesystem::path symlink = temp.path / "helper-link";
    REQUIRE(::symlink(helper.c_str(), symlink.c_str()) == 0);
    ScopedEnvironmentVariable symlink_override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", symlink.string()};
    gv::QualifiedMetalArchiveHelperProcessStore failed_store;
    CHECK_FALSE(failed_store.load_once());
    CHECK_FALSE(failed_store.load_once());
    CHECK(failed_store.qualification_attempt_count() == 1u);
    CHECK(failed_store.filesystem_hash_count() == 0u);

    REQUIRE(::chmod(helper.c_str(), 0702) == 0);
    ScopedEnvironmentVariable writable_override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};
    gv::QualifiedMetalArchiveHelperProcessStore writable_store;
    CHECK_FALSE(writable_store.load_once());
    CHECK(writable_store.qualification_attempt_count() == 1u);
    CHECK(writable_store.filesystem_hash_count() == 0u);
#endif
}

TEST_CASE("archive helper override is explicitly test-only",
          "[shader_cache][metal-archive][qualification][helper]") {
#if !defined(__APPLE__)
    SKIP("archive helper qualification is Apple-only");
#else
    TempDir temp;
    const std::filesystem::path helper = temp.path / "helper.sh";
    REQUIRE(gv::durable_write_file(helper, bytes_of("#!/bin/sh\nexit 1\n")));
    REQUIRE(::chmod(helper.c_str(), 0700) == 0);
    ScopedEnvironmentVariable test_only{
        "BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY", "0"};
    ScopedEnvironmentVariable override{
        "BIOSEQ_GPU_ARCHIVE_HELPER", helper.string()};
    gv::QualifiedMetalArchiveHelperProcessStore store;
    CHECK_FALSE(store.load_once());
    CHECK_FALSE(store.load_once());
    CHECK(store.qualification_attempt_count() == 1u);
    CHECK(store.filesystem_hash_count() == 0u);
#endif
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
    write_fake_archive_qualification(
        gv::cache_binary_archive_path(temp.path, key), fake_archive,
        key, metallib, entry);
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

    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t reserved_before = admission.reserved_bytes();
    {
        const auto module_code = gv::make_moltenvk_metallib_module_code(
            gv::cache_metallib_path(temp.path, key));
        REQUIRE(module_code);
        CHECK(module_code->admission_count == 1u);
        CHECK(module_code->peak_admitted_bytes
              >= module_code->retained_capacity_bytes);
        CHECK(module_code->retained_capacity_bytes > 0u);
        CHECK_FALSE(module_code->admission_saturated);
        CHECK(module_code->permit.bytes()
              == module_code->retained_capacity_bytes);
        CHECK(module_code->header_size_bytes
              == gv::moltenvk_msl_spirv_header_size());
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
    CHECK(admission.reserved_bytes() == reserved_before);
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
    write_fake_archive_qualification(
        gv::cache_binary_archive_path(temp.path, key), archive,
        key, metallib, entry);

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

namespace {

[[nodiscard]] gv::RuntimeMetalArchiveConfiguration
accepted_runtime_metal_archive_configuration() {
    TempDir temp;
    constexpr std::string_view digest = "0123456789abcdef";
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::chain_tiled_dp_u64, 32u, 32u, 0u);
    const std::array keys{key};
    REQUIRE(gv::cache_binary_archive_paths(temp.path, key).size() == 2u);
    write_fake_sealed_shader_artifacts(temp.path, keys);
    REQUIRE(gv::seal_shader_cache_manifest(temp.path, digest, keys));
    auto loaded = gv::load_validated_sealed_shader_cache_seed(
        temp.path, digest, keys);
    REQUIRE(loaded);
    const auto seed = std::make_shared<const gv::SealedShaderCacheSeedImage>(
        std::move(*loaded));
    gv::RuntimeMetalArchiveConfiguration configuration{};
    configuration.seed_image = seed;
    configuration.vulkan_device_uuid[0] = 0x42u;
    configuration.metal_device_registry_id = 0x1234u;
    for (std::size_t ordinal = 0u;
         ordinal < seed->artifacts().size(); ++ordinal) {
        const auto& artifact = seed->artifacts()[ordinal];
        if (artifact.descriptor.kind
            != gv::SealedShaderCacheArtifactKind::metal_binary_archive) {
            continue;
        }
        const auto* qualification =
            gv::sealed_shader_cache_detail::find_artifact_image(
                seed->artifacts(),
                gv::SealedShaderCacheArtifactKind::
                    metal_binary_archive_qualification,
                artifact.descriptor.key_index,
                artifact.descriptor.filename + ".qualification");
        REQUIRE(qualification != nullptr);
        const auto parsed = gv::sealed_shader_cache_detail::
            parse_metal_archive_qualification_receipt(qualification->bytes);
        REQUIRE(parsed);
        if (configuration.qualification_receipt_count == 0u) {
            configuration.qualification_version = parsed->wire_version;
            configuration.helper_protocol_version =
                parsed->helper_protocol_version;
            configuration.helper_build_identity =
                parsed->helper_build_identity;
            configuration.helper_source = parsed->helper_source;
            configuration.helper_executable_size =
                parsed->helper_executable_size;
            configuration.helper_executable_sha256 =
                parsed->helper_executable_sha256;
            configuration.helper_metal_registry_id =
                parsed->helper_metal_registry_id;
        }
        ++configuration.qualification_receipt_count;
        configuration.archives.push_back(gv::RuntimeMetalArchiveSpec{
            .artifact_ordinal = static_cast<std::uint32_t>(ordinal),
            .artifact_filename = artifact.descriptor.filename,
            .size = artifact.descriptor.size,
            .sha256 = artifact.descriptor.sha256,
            .negative_error_domain = parsed->negative_error.domain,
            .negative_error_code = parsed->negative_error.code,
        });
    }
    return configuration;
}

[[nodiscard]] gv::RuntimeMetalArchivePipelineAudit
accepted_runtime_metal_archive_audit() {
    const auto configuration = accepted_runtime_metal_archive_configuration();
    const auto archive_identity = gv::runtime_metal_archive_set_identity(
        configuration.archives);
    gv::RuntimeMetalArchivePipelineAudit audit{};
    audit.receipt_identity = 7u;
    audit.scope_identity = 8u;
    const auto& manifest = configuration.seed_image->manifest();
    const auto& seed_receipt = configuration.seed_image->receipt();
    audit.cache_digest = manifest.cache_digest;
    audit.seed_sha256 = manifest.seed_sha256;
    audit.seed_size = manifest.seed_size;
    audit.manifest_checksum = manifest.manifest_checksum;
    audit.seed_receipt_identity = seed_receipt.identity();
    audit.seed_load_count = seed_receipt.load_count();
    audit.seed_manifest_read_count = seed_receipt.manifest_read_count();
    audit.seed_artifact_read_count = seed_receipt.artifact_read_count();
    audit.seed_manifest_bytes = seed_receipt.manifest_bytes();
    audit.seed_artifact_bytes = seed_receipt.artifact_bytes();
    audit.seed_load_ns = seed_receipt.load_ns();
    audit.seed_admission_count = seed_receipt.admission_count();
    audit.seed_peak_admitted_bytes = seed_receipt.peak_admitted_bytes();
    audit.seed_retained_capacity_bytes =
        seed_receipt.retained_capacity_bytes();
    audit.seed_admission_saturated = seed_receipt.admission_saturated();
    audit.shader_key = gv::make_shader_key(
        gv::ShaderKind::chain_tiled_dp_u64, 32u, 32u, 0u);
    audit.expected_entry_name = "main0";
    audit.observed_entry_name = audit.expected_entry_name;
    audit.matched_descriptor_functions = 1u;
    audit.expected_archives = archive_identity;
    audit.loaded_archives = archive_identity;
    audit.archive_pre_post_identity_checks = archive_identity.archive_count;
    audit.pipeline_pre_identity_checks = 0u;
    audit.pipeline_post_identity_checks = 0u;
    audit.command_interpose_installed = true;
    audit.command_interpose_immutable_fail_closed = true;
    audit.configured_vulkan_device_uuid = configuration.vulkan_device_uuid;
    audit.runtime_vulkan_device_uuid = configuration.vulkan_device_uuid;
    audit.api_version = 0x00403000u;
    audit.driver_version = 0x01020304u;
    audit.vendor_id = 0x0000106bu;
    audit.device_id = 0x00000001u;
    audit.pipeline_cache_uuid[0] = 0x42u;
    audit.pipeline_cache_uuid[15] = 0x24u;
    audit.cache_qualification = gv::RuntimeMetalArchiveProcessQualification{
        .version = 2u,
        .archive_set = archive_identity,
        .receipt_count = configuration.qualification_receipt_count,
        .hash_passes = 1u,
        .hashed_bytes = archive_identity.total_size,
        .private_files_created = archive_identity.archive_count,
        .private_files_unlinked = archive_identity.archive_count,
        .live_private_paths = 0u,
        .pipeline_filesystem_hash_passes = 0u,
        .pipeline_filesystem_hashed_bytes = 0u,
        .helper_protocol_version = configuration.helper_protocol_version,
        .helper_build_identity = "bioseq_gpu_archive_builder_v2",
        .helper_source = "installed-sibling",
        .helper_executable_size = configuration.helper_executable_size,
        .helper_executable_sha256 =
            configuration.helper_executable_sha256,
        .helper_metal_registry_id =
            configuration.helper_metal_registry_id,
        .vulkan_device_uuid = audit.runtime_vulkan_device_uuid,
        .api_version = audit.api_version,
        .driver_version = audit.driver_version,
        .vendor_id = audit.vendor_id,
        .device_id = audit.device_id,
        .pipeline_cache_uuid = audit.pipeline_cache_uuid,
        .seed_receipt_identity = audit.seed_receipt_identity,
        .seed_sha256 = audit.seed_sha256,
        .seed_size = audit.seed_size,
        .manifest_checksum = audit.manifest_checksum,
        .saturated = false,
    };
    audit.cache_qualification.identity =
        gv::runtime_metal_archive_process_qualification_identity(
            audit.cache_qualification);
    audit.pipeline_create_ns = 17u;
    audit.expected_metal_registry_id =
        configuration.metal_device_registry_id;
    audit.command_loader_metal_registry_id =
        configuration.metal_device_registry_id;
    audit.command_source_methods_present = 2u;
    audit.command_source_wrapper_methods = 2u;
    audit.loader_metal_registry_id = configuration.metal_device_registry_id;
    audit.intercepted_metal_registry_id =
        configuration.metal_device_registry_id;
    audit.scoped_interceptions = 1u;
    audit.descriptor_replacements = 1u;
    audit.fail_on_miss_enforcements = 1u;
    audit.same_descriptor_negative_controls = 1u;
    audit.negative_control_misses = 1u;
    audit.expected_negative_error_domain =
        configuration.archives.front().negative_error_domain;
    audit.expected_negative_error_code =
        configuration.archives.front().negative_error_code;
    audit.observed_negative_error_domain =
        audit.expected_negative_error_domain;
    audit.observed_negative_error_code = audit.expected_negative_error_code;
    audit.original_sync_negative_calls = 1u;
    audit.actual_sync_calls = 1u;
    audit.actual_async_calls = 0u;
    audit.outer_vulkan_pipeline_succeeded = true;
    return audit;
}

}  // namespace

TEST_CASE("immutable live gate uses cached scalar configuration evidence",
          "[vulkan][metal-archive-receipt][environment][live-gate]") {
    const auto before = gv::runtime_metal_archive_live_gate_evidence();
    REQUIRE_FALSE(before.installed_accepted);
    for (std::size_t iteration = 0u; iteration < 8u; ++iteration) {
        gv::probe_immutable_metal_policy_live_for_test();
    }
    const auto after = gv::runtime_metal_archive_live_gate_evidence();
    CHECK(after.install_validation_count == before.install_validation_count);
    CHECK(after.live_gate_calls == before.live_gate_calls + 8u);
    CHECK(after.live_configuration_scan_count == 0u);
}

TEST_CASE("immutable Metal archive configuration binds the exact ordered set",
          "[vulkan][metal-archive-receipt]") {
    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    auto accepted = accepted_runtime_metal_archive_configuration();
    REQUIRE(accepted.accepted());
    const std::uint64_t retained_seed_bytes =
        accepted.seed_image->receipt().retained_capacity_bytes();
    const std::uint64_t reserved_with_seed = admission.reserved_bytes();
    REQUIRE(reserved_with_seed >= retained_seed_bytes);
    // The first qualification also process-retains the installed helper. Keep
    // that intentional singleton reservation in the baseline while proving
    // that this configuration's seed and derived image release independently.
    const std::uint64_t process_qualification_baseline =
        reserved_with_seed - retained_seed_bytes;
    const auto identity = gv::runtime_metal_archive_set_identity(
        accepted.archives);
    CHECK(identity.accepted());
    CHECK(identity.archive_count == 2u);
    CHECK(identity.total_size > 0u);
    {
        const auto admitted =
            gv::make_runtime_metal_archive_configuration_image(
                accepted.seed_image,
                accepted.vulkan_device_uuid,
                accepted.metal_device_registry_id);
        REQUIRE(admitted);
        REQUIRE((*admitted)->accepted());
        CHECK((*admitted)->configuration() == accepted);
        CHECK((*admitted)->retained_capacity_bytes() > 0u);
        CHECK((*admitted)->retained_capacity_bytes()
              <= (*admitted)->peak_admitted_bytes());
    }
    CHECK(admission.reserved_bytes() == reserved_with_seed);

    SECTION("configuration rejects missing immutable identities") {
        auto invalid = accepted;
        invalid.seed_image.reset();
        CHECK_FALSE(invalid.accepted());

        // Device identities may be unknown to the supervisor before Runtime
        // selects VkPhysicalDevice/MTLDevice. Zero is an explicit request for
        // Runtime to seal the actual identities; the receipt never accepts
        // zero after that sealing step.
        auto runtime_sealed = accepted;
        runtime_sealed.vulkan_device_uuid = {};
        runtime_sealed.metal_device_registry_id = 0u;
        CHECK(runtime_sealed.accepted());
    }

    SECTION("archive entries are retained-image-bound and strictly ordered") {
        auto invalid = accepted;
        invalid.archives.clear();
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.archives[0].artifact_filename += ".other";
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.archives[0].artifact_ordinal;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.archives[0].size = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.archives[0].sha256 = {};
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.archives[0].negative_error_domain.clear();
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.archives[1].negative_error_code;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.archives[1].negative_error_domain += ".other";
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        std::swap(invalid.archives[0], invalid.archives[1]);
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.archives[1] = invalid.archives[0];
        CHECK_FALSE(invalid.accepted());
    }

    SECTION("helper qualification metadata is exact") {
        auto invalid = accepted;
        invalid.qualification_version = 1u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        --invalid.qualification_receipt_count;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.helper_protocol_version = 1u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.helper_build_identity += ".other";
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.helper_source = "test-env-override";
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.helper_executable_size;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.helper_executable_sha256 = {};
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.helper_metal_registry_id = 0u;
        CHECK_FALSE(invalid.accepted());
    }

    SECTION("every byte identity component changes the ordered set") {
        auto changed = accepted;
        changed.archives[0].artifact_filename += ".other";
        CHECK(gv::runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        ++changed.archives[0].artifact_ordinal;
        CHECK(gv::runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        ++changed.archives[0].size;
        CHECK(gv::runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        changed.archives[0].sha256[1] = 0x77u;
        CHECK(gv::runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        changed.archives[0].negative_error_domain += ".other";
        CHECK(gv::runtime_metal_archive_set_identity(changed.archives)
              != identity);
        changed = accepted;
        ++changed.archives[0].negative_error_code;
        CHECK(gv::runtime_metal_archive_set_identity(changed.archives)
              != identity);
    }
    accepted = {};
    CHECK(admission.reserved_bytes() == process_qualification_baseline);
}

TEST_CASE("immutable Metal archive pipeline receipt is strict and retained",
          "[vulkan][metal-archive-receipt]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::RuntimeMetalArchivePipelineReceipt>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::RuntimeMetalArchivePipelineReceipt>);

    const auto accepted = accepted_runtime_metal_archive_audit();
    REQUIRE(accepted.accepted());
    CHECK(gv::RuntimeMetalArchivePipelineReceipt::
          self_test_move_and_reuse_contract(accepted));

    SECTION("exact expected and loaded archive sets must agree") {
        auto invalid = accepted;
        ++invalid.loaded_archives.archive_count;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.loaded_archives.total_size;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.loaded_archives.ordered_checksum;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        --invalid.archive_pre_post_identity_checks;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        --invalid.pipeline_pre_identity_checks;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        --invalid.pipeline_post_identity_checks;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.unsealed_metal_source_requests = 1u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.metal_source_fallback_calls = 1u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.manifest_checksum = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.cache_digest.clear();
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.cache_digest = "0000000000000000";
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.cache_digest[0] = 'A';
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.seed_sha256 = {};
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.seed_size = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.seed_receipt_identity = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.seed_load_count = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.seed_manifest_read_count = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.seed_artifact_read_count = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.seed_artifact_bytes;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.seed_load_ns = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.seed_admission_count = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.seed_retained_capacity_bytes =
            invalid.seed_peak_admitted_bytes + 1u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.seed_admission_saturated = true;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.shader_key = {};
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.observed_entry_name += ".other";
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.matched_descriptor_functions = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.command_interpose_installed = false;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.command_interpose_immutable_fail_closed = false;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.command_loader_metal_registry_id;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        --invalid.command_source_methods_present;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        --invalid.command_source_wrapper_methods;
        CHECK_FALSE(invalid.accepted());
    }

    SECTION("Vulkan loader and intercepted Metal device identities are exact") {
        auto invalid = accepted;
        invalid.runtime_vulkan_device_uuid[1] = 0x44u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.api_version = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.driver_version = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.pipeline_cache_uuid = {};
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.pipeline_create_ns = 0u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.loader_metal_registry_id;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.intercepted_metal_registry_id;
        CHECK_FALSE(invalid.accepted());
    }

    SECTION("every scoped call replaces archives enforces miss and runs control") {
        auto invalid = accepted;
        --invalid.descriptor_replacements;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        --invalid.fail_on_miss_enforcements;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        --invalid.same_descriptor_negative_controls;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        --invalid.negative_control_misses;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        --invalid.original_sync_negative_calls;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.actual_async_calls;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.scoped_interceptions;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.observed_negative_error_domain += ".other";
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        ++invalid.observed_negative_error_code;
        CHECK_FALSE(invalid.accepted());
    }

    SECTION("outside nested sticky and failed outer execution fail closed") {
        auto invalid = accepted;
        invalid.outside_scope_interceptions = 1u;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.nested_scope = true;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.sticky_failure = true;
        CHECK_FALSE(invalid.accepted());
        invalid = accepted;
        invalid.outer_vulkan_pipeline_succeeded = false;
        CHECK_FALSE(invalid.accepted());
    }

    SECTION("process qualification equations reject every filesystem shortcut") {
        const auto reject = [&accepted](auto mutate) {
            auto invalid = accepted;
            mutate(invalid.cache_qualification);
            invalid.cache_qualification.identity =
                gv::runtime_metal_archive_process_qualification_identity(
                    invalid.cache_qualification);
            CHECK_FALSE(invalid.accepted());
        };
        reject([](auto& value) { value.hash_passes = 2u; });
        reject([](auto& value) { ++value.hashed_bytes; });
        reject([](auto& value) { --value.private_files_created; });
        reject([](auto& value) { --value.private_files_unlinked; });
        reject([](auto& value) { value.live_private_paths = 1u; });
        reject([](auto& value) {
            value.pipeline_filesystem_hash_passes = 1u;
        });
        reject([](auto& value) {
            value.pipeline_filesystem_hashed_bytes = 1u;
        });
        reject([](auto& value) { value.helper_source = "test-env-override"; });
        reject([](auto& value) { value.helper_metal_registry_id = 0u; });
        reject([](auto& value) { value.vulkan_device_uuid[0] ^= 0x80u; });
        reject([](auto& value) { ++value.api_version; });
        reject([](auto& value) { ++value.driver_version; });
        reject([](auto& value) { ++value.vendor_id; });
        reject([](auto& value) { ++value.device_id; });
        reject([](auto& value) { value.pipeline_cache_uuid[0] ^= 0x80u; });
        reject([](auto& value) { value.seed_sha256[0] ^= 0x80u; });
        reject([](auto& value) { ++value.seed_size; });
        reject([](auto& value) { ++value.manifest_checksum; });

        auto invalid_identity = accepted;
        invalid_identity.cache_qualification.identity = 0u;
        CHECK_FALSE(invalid_identity.accepted());
    }
}

TEST_CASE("runtime pipeline PREPARED and USED receipts are causal and one-shot",
          "[vulkan][metal-archive-receipt][pipeline-use-proof][admission]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::RuntimePipelineOperationProof>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::RuntimePipelineOperationProof>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::PersistedShapeV5CoordinateAcceptedOperation>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::PersistedShapeV5CoordinateAcceptedOperation>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::RuntimeGpuCacheMetricsDelta>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::RuntimeGpuCacheMetricsDelta>);

    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const auto base_audit = accepted_runtime_metal_archive_audit();
    REQUIRE(base_audit.accepted());
    // Constructing the first audit also once-qualifies and process-retains the
    // installed helper executable. That reservation intentionally lives until
    // process shutdown, so leak checks begin after this process-wide baseline.
    const std::uint64_t reserved_after_process_qualification =
        admission.reserved_bytes();
    CHECK(gv::RuntimePipelineOperationProof::
          self_test_causal_collector_contract(
              base_audit));
    CHECK(admission.reserved_bytes()
          == reserved_after_process_qualification);
    CHECK(gv::Runtime::
          self_test_persisted_shape_v5_finalizer_preclaim_contract(
              base_audit));
    CHECK(admission.reserved_bytes()
          == reserved_after_process_qualification);
    CHECK(gv::Runtime::self_test_gpu_cache_partial_delta_contract(
        base_audit));
    CHECK(admission.reserved_bytes()
          == reserved_after_process_qualification);
}

TEST_CASE("runtime GPU environment identity is canonical and byte exact",
          "[vulkan][metal-archive-receipt][environment]") {
    gv::VulkanFeatureSet features{};
    features.api_version = 0x01020304u;
    features.driver_version = 0xa0b0c0d0u;
    features.vendor_id = 0x0000106bu;
    features.device_id = 0x0000002au;
    for (std::size_t i = 0u; i < features.pipeline_cache_uuid.size(); ++i) {
        features.pipeline_cache_uuid[i] = static_cast<std::uint8_t>(i);
    }
    const auto identity = gv::canonical_vulkan_driver_identity(features);
    CHECK(std::string(identity.data())
          == "api=01020304;driver=a0b0c0d0;vendor=0000106b;"
             "device=0000002a;pcuuid=000102030405060708090a0b0c0d0e0f");
    const auto terminator = std::find(
        identity.begin(), identity.end(), '\0');
    REQUIRE(terminator != identity.end());
    CHECK(std::all_of(
        std::next(terminator), identity.end(),
        [](char byte) { return byte == '\0'; }));

    auto changed = features;
    ++changed.api_version;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    ++changed.driver_version;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    ++changed.vendor_id;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    ++changed.device_id;
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
    changed = features;
    std::swap(changed.pipeline_cache_uuid[0],
              changed.pipeline_cache_uuid[1]);
    CHECK(gv::canonical_vulkan_driver_identity(changed) != identity);
}

TEST_CASE("runtime GPU cache delta is bounded by its pre-finalizer maximum",
          "[vulkan][metal-archive-receipt][cache-metrics]") {
    const gv::RuntimeGpuCacheMetricsAudit maximum{
        .token_identity = 77u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = 4096u,
        .gpu_pipeline_cache_load_ns = 11u,
        .gpu_pipeline_creates = 8u,
        .gpu_pipeline_creation_ns = 88u,
    };
    const gv::RuntimeGpuCacheMetricsAudit first = maximum;
    const gv::RuntimeGpuCacheMetricsAudit later{
        .token_identity = 77u,
    };
    REQUIRE(maximum.accepted());
    CHECK(first.componentwise_bounded_by(maximum));
    CHECK(later.componentwise_bounded_by(maximum));
    auto invalid = later;
    invalid.token_identity = 78u;
    CHECK_FALSE(invalid.componentwise_bounded_by(maximum));
    invalid = first;
    ++invalid.gpu_pipeline_creation_ns;
    CHECK_FALSE(invalid.componentwise_bounded_by(maximum));
    invalid = first;
    invalid.saturated = true;
    CHECK_FALSE(invalid.componentwise_bounded_by(maximum));
}

TEST_CASE("Shape-V5 radix pass count is exact at byte boundaries",
          "[vulkan][pipeline-use-proof][shape-v5][radix]") {
    const auto passes = [](std::uint64_t target_end) {
        return gv::runtime_shape_v5_radix_pass_count(
            0u, target_end + 1u);
    };
    CHECK(passes(0u) == 1u);                      // 1 coord + strand
    CHECK(passes(0x7fu) == 1u);                   // 7 coord + strand
    CHECK(passes(0x80u) == 2u);                   // 8 coord + strand
    CHECK(passes(0xffu) == 2u);                   // 8 coord + strand
    CHECK(passes(0x100u) == 2u);                  // 9 coord + strand
    CHECK(passes(0x7fffu) == 2u);                 // 15 coord + strand
    CHECK(passes(0x8000u) == 3u);                 // 16 coord + strand
    CHECK(passes(0xffffu) == 3u);                 // 16 coord + strand
    CHECK(passes(0x10000u) == 3u);                // 17 coord + strand
    CHECK(passes(9'999'999u) == 4u);              // FAST_LONG: 24 + strand
    CHECK(passes(499'999'999u) == 4u);            // XXL_HIFI: 29 + strand
    CHECK(passes(std::uint64_t{1} << 62u) == 8u); // 63 coord + strand
    CHECK_FALSE(gv::runtime_shape_v5_radix_pass_count(0u, 0u));
    CHECK_FALSE(gv::runtime_shape_v5_radix_pass_count(
        0u, (std::uint64_t{1} << 63u) + 1u));
    CHECK_FALSE(gv::runtime_shape_v5_radix_pass_count(
        (std::numeric_limits<std::uint64_t>::max)(), 2u));
}

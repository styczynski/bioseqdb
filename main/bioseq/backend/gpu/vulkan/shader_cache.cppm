/**
 * @file shader_cache.cppm
 * @brief Supervisor-built Vulkan shader metallib cache for MoltenVK backends.
 */
module;
#include "common.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#if __has_include(<MoltenVK/mvk_private_api.h>)
#include <MoltenVK/mvk_private_api.h>
#elif __has_include(<mvk_private_api.h>)
#include <mvk_private_api.h>
#else
using MVKMSLSPIRVHeader = std::uint32_t;
inline constexpr std::uint32_t kMVKMagicNumberMSLCompiledCode = 0x19981215u;
#endif
#else
using MVKMSLSPIRVHeader = std::uint32_t;
inline constexpr std::uint32_t kMVKMagicNumberMSLCompiledCode = 0x19981215u;
#endif

#include <spirv_cross/spirv_msl.hpp>
#include <vulkan/vulkan.h>

extern char** environ;

export module bioseq.gpu.vulkan.shader_cache;

import bioseq.common.hashing;
import bioseq.common.mlogging;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;

export namespace bioseq::gpu::vulkan {

template <class... Args>
[[nodiscard]] inline std::string shader_cache_message(Args&&... args)
{
    std::ostringstream out;
    (out << ... << std::forward<Args>(args));
    return out.str();
}

enum class ShaderCacheErrc : std::uint32_t {
    Ok = 0,
    NoToolchain,
    Glslang,
    SpirvToMsl,
    MetalSpawn,
    MetalExit,
    MetallibWrite,
    MetaIo,
    AbiScan,
    DlsymMissing,
    QueueFull,
    SupervisorGone,
    Timeout
};

struct ShaderCacheMeta {
    std::uint32_t magic = 0xB10C0CACu;
    std::uint32_t version = 1;
    ShaderVariantKey key{};
    std::uint32_t wg_x = 1;
    std::uint32_t wg_y = 1;
    std::uint32_t wg_z = 1;
    std::uint32_t entry_len = 0;
};
static_assert(std::is_standard_layout_v<ShaderCacheMeta>);

struct ShaderModuleLoad {
    VkShaderModule module = VK_NULL_HANDLE;
    std::string entry_name;
    std::uint32_t wg_x = 1;
    std::uint32_t wg_y = 1;
    std::uint32_t wg_z = 1;
};

struct MoltenVkShaderModuleCode {
    std::vector<std::uint32_t> words;
    std::size_t code_size_bytes = 0;
    std::size_t header_size_bytes = sizeof(MVKMSLSPIRVHeader);
};

struct MslVersion {
    std::uint32_t major = 3;
    std::uint32_t minor = 2;
    std::uint32_t patch = 0;
};

struct ShaderCacheBuildInputs {
    VulkanStaticVariantInputs variants{};
    MslVersion msl{};
    std::string moltenvk_build = "conservative";
    std::string device_uuid = "conservative";
};

template <class Service>
concept is_runtime_compile_service = requires(Service& service, ShaderVariantKey key) {
    { service.ensure_variant_compiled(key) } -> std::same_as<std::expected<void, std::string>>;
};

[[nodiscard]] inline std::filesystem::path cache_metallib_path(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key)
{
    return cache_dir / (cache_file_stem(key) + ".metallib");
}

[[nodiscard]] inline std::filesystem::path cache_binary_archive_path(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key)
{
    return cache_dir / (cache_file_stem(key) + ".metalar");
}

[[nodiscard]] inline std::vector<std::filesystem::path> cache_binary_archive_paths(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key)
{
    constexpr std::uint32_t conservative_moltenvk_cap = 256u;
    const ShaderWorkgroupSize workgroup = expected_shader_workgroup_size(key);
    const std::uint64_t exact_threads =
        static_cast<std::uint64_t>(workgroup.x)
        * static_cast<std::uint64_t>(workgroup.y)
        * static_cast<std::uint64_t>(workgroup.z);
    std::vector<std::filesystem::path> paths{cache_binary_archive_path(cache_dir, key)};
    if (exact_threads != conservative_moltenvk_cap) {
        paths.push_back(cache_dir / (cache_file_stem(key) + ".max_threads_"
                                     + std::to_string(conservative_moltenvk_cap)
                                     + ".metalar"));
    }
    return paths;
}

[[nodiscard]] inline std::filesystem::path cache_meta_path(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key)
{
    return cache_dir / (cache_file_stem(key) + ".meta");
}

[[nodiscard]] inline std::filesystem::path moltenvk_command_metallib_path(
    const std::filesystem::path& cache_dir)
{
    return cache_dir / "moltenvk_command.metallib";
}

[[nodiscard]] inline std::string_view moltenvk_command_placeholder_msl() noexcept
{
    return R"msl(#include <metal_stdlib>
using namespace metal;

kernel void bioseq_moltenvk_command_placeholder() {}
)msl";
}

[[nodiscard]] inline std::string system_uname_digest_component()
{
    utsname info{};
    if (::uname(&info) != 0) {
        return "uname-unavailable";
    }
    return shader_cache_message(
        info.sysname, ' ', info.nodename, ' ', info.release, ' ',
        info.version, ' ', info.machine);
}

[[nodiscard]] inline std::expected<std::string, std::string> run_capture(
    std::span<const std::string> args)
{
    if (args.empty()) {
        return std::unexpected("empty command");
    }
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        return std::unexpected(shader_cache_message("pipe failed: errno ", errno));
    }

    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    if (rc != 0) {
        ::close(pipe_fds[0]);
        return std::unexpected(shader_cache_message(args[0], " spawn failed: errno ", rc));
    }

    std::string output;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        ::close(pipe_fds[0]);
        return std::unexpected(shader_cache_message(args[0], " read failed: errno ", errno));
    }
    ::close(pipe_fds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return std::unexpected(shader_cache_message(args[0], " waitpid failed: errno ", errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " exited with status ",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1, ": ", output));
    }
    return output;
}

[[nodiscard]] inline std::string xcrun_executable()
{
    if (::access("/usr/bin/xcrun", X_OK) == 0) {
        return "/usr/bin/xcrun";
    }
    return "xcrun";
}

[[nodiscard]] inline std::string probe_metal_version()
{
    const std::array<std::string, 4> args{
        xcrun_executable(), "-sdk", "macosx", "metal"};
    std::vector<std::string> version_args(args.begin(), args.end());
    version_args.push_back("--version");
    auto result = run_capture(version_args);
    return result ? *result : std::string{"xcrun metal unavailable"};
}

[[nodiscard]] inline std::string shader_cache_digest_hex(
    const ShaderCacheBuildInputs& inputs)
{
    std::uint64_t hash = bioseq::common::fnv1a_64("bioseq-shader-cache-v1");
    hash = bioseq::common::hash_combine_64(hash, inputs.moltenvk_build);
    hash = bioseq::common::hash_combine_64(hash, inputs.device_uuid);
    hash = bioseq::common::hash_combine_64(hash, probe_metal_version());
    hash = bioseq::common::hash_combine_64(hash, system_uname_digest_component());
    hash = bioseq::common::hash_combine_64(hash, inputs.msl.major);
    hash = bioseq::common::hash_combine_64(hash, inputs.msl.minor);
    hash = bioseq::common::hash_combine_64(hash, inputs.msl.patch);
    hash = bioseq::common::hash_combine_64(hash, moltenvk_command_placeholder_msl());
    hash = bioseq::common::hash_combine_64(hash, "bioseq-metal-binary-archive-v2");
    // SPIR-V -> MSL resource indices are pipeline ABI, even when the GLSL
    // shader body itself is unchanged.  Keep an explicit transform revision
    // and the concrete layout in the digest so an incompatible metallib or
    // Metal binary archive is never reused under the old cache directory.
    hash = bioseq::common::hash_combine_64(
        hash, "bioseq-spirv-to-msl-resource-abi-v2");

    for_each_static_shader_variant(inputs.variants, [&hash](const ShaderVariantKey& key) {
        const ShaderKind kind = checked_shader_kind(key.kind);
        const DescriptorLayoutKind descriptor_layout = descriptor_layout_for(kind);
        hash = bioseq::common::hash_combine_64(hash, key.kind);
        hash = bioseq::common::hash_combine_64(hash, key.tile_size);
        hash = bioseq::common::hash_combine_64(hash, key.local_size);
        hash = bioseq::common::hash_combine_64(hash, key.max_span);
        hash = bioseq::common::hash_combine_64(
            hash, static_cast<std::uint32_t>(descriptor_layout));
        hash = bioseq::common::hash_combine_64(
            hash, descriptor_binding_count(descriptor_layout));
        hash = bioseq::common::hash_combine_64(
            hash, push_constant_size(kind));
        hash = bioseq::common::hash_combine_64(
            hash, shader_body_for_digest(kind));
    });
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

/// Content identity for an explicitly requested grouped32 ABI2 preseed. This
/// does not participate in the production supervisor cache digest or AOT set;
/// callers can record/compare it when measuring an inactive preseed operation.
[[nodiscard]] inline std::string grouped32_inactive_shader_set_digest_hex()
{
    std::uint64_t hash = bioseq::common::fnv1a_64(
        "bioseq-grouped32-inactive-preseed-v1");
    for_each_inactive_grouped32_shader_variant(
        [&hash](const ShaderVariantKey& key) {
            const ShaderKind kind = checked_shader_kind(key.kind);
            const auto layout = descriptor_layout_for(kind);
            hash = bioseq::common::hash_combine_64(hash, key.kind);
            hash = bioseq::common::hash_combine_64(hash, key.tile_size);
            hash = bioseq::common::hash_combine_64(hash, key.local_size);
            hash = bioseq::common::hash_combine_64(hash, key.max_span);
            hash = bioseq::common::hash_combine_64(
                hash, static_cast<std::uint32_t>(layout));
            hash = bioseq::common::hash_combine_64(
                hash, descriptor_binding_count(layout));
            hash = bioseq::common::hash_combine_64(
                hash, push_constant_size(kind));
            hash = bioseq::common::hash_combine_64(
                hash, shader_body_for_digest(kind));
        });
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

[[nodiscard]] inline std::expected<void, std::string> fsync_parent_dir(
    const std::filesystem::path& path)
{
    const std::filesystem::path parent = path.parent_path();
    const int fd = ::open(parent.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::unexpected(shader_cache_message(
            "open dir ", parent.string(), " failed: errno ", errno));
    }
    const int rc = ::fsync(fd);
    const int saved = errno;
    ::close(fd);
    if (rc != 0) {
        return std::unexpected(shader_cache_message(
            "fsync dir ", parent.string(), " failed: errno ", saved));
    }
    return {};
}

[[nodiscard]] inline std::expected<void, std::string> write_all_fd(
    int fd,
    std::span<const std::byte> bytes)
{
    const std::byte* cursor = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        const ssize_t n = ::write(fd, cursor, remaining);
        if (n > 0) {
            cursor += n;
            remaining -= static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return std::unexpected(shader_cache_message("write failed: errno ", errno));
    }
    return {};
}

[[nodiscard]] inline std::expected<void, std::string> durable_write_file(
    const std::filesystem::path& final_path,
    std::span<const std::byte> bytes)
{
    const std::filesystem::path tmp = final_path.string() + ".tmp";
    const int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        return std::unexpected(shader_cache_message(
            "open ", tmp.string(), " failed: errno ", errno));
    }
    auto write_result = write_all_fd(fd, bytes);
    if (!write_result) {
        ::close(fd);
        return write_result;
    }
    if (::fsync(fd) != 0) {
        const int saved = errno;
        ::close(fd);
        return std::unexpected(shader_cache_message(
            "fsync ", tmp.string(), " failed: errno ", saved));
    }
    if (::close(fd) != 0) {
        return std::unexpected(shader_cache_message(
            "close ", tmp.string(), " failed: errno ", errno));
    }
    if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
        return std::unexpected(shader_cache_message(
            "rename ", tmp.string(), " -> ", final_path.string(),
            " failed: errno ", errno));
    }
    return fsync_parent_dir(final_path);
}

[[nodiscard]] inline std::expected<void, std::string> durable_rename_existing(
    const std::filesystem::path& tmp,
    const std::filesystem::path& final_path)
{
    const int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::unexpected(shader_cache_message(
            "open ", tmp.string(), " failed: errno ", errno));
    }
    if (::fsync(fd) != 0) {
        const int saved = errno;
        ::close(fd);
        return std::unexpected(shader_cache_message(
            "fsync ", tmp.string(), " failed: errno ", saved));
    }
    ::close(fd);
    if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
        return std::unexpected(shader_cache_message(
            "rename ", tmp.string(), " -> ", final_path.string(),
            " failed: errno ", errno));
    }
    return fsync_parent_dir(final_path);
}

[[nodiscard]] inline std::expected<void, std::string> spawn_wait(
    std::span<const std::string> args)
{
    auto result = run_capture(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

[[nodiscard]] inline std::expected<void, std::string> validate_spirv_layout(
    const ShaderVariantKey& key,
    std::span<const std::uint32_t> spirv)
{
    const SpirvLayoutScan scan = scan_spirv_layout(spirv);
    const ShaderKind kind = checked_shader_kind(key.kind);
    if (kind == ShaderKind::chain_tiled_dp_u64) {
        if (!scan.has_array_stride(24) || !scan.has_struct_offsets({0u, 8u, 16u, 20u})) {
            return std::unexpected("SPIR-V chain_tiled_dp_u64 ABI mismatch");
        }
    }
    if (kind == ShaderKind::seed_lookup_anchors
        && (!scan.has_binding(0) || !scan.has_binding(1)
            || !scan.has_binding(2) || !scan.has_binding(3)
            || !scan.has_binding(4) || !scan.has_binding(5)
            || !scan.has_array_stride(24) || !scan.has_array_stride(8)
            || !scan.has_struct_offsets({0u, 8u, 16u, 20u}))) {
        return std::unexpected("SPIR-V seed_lookup_anchors ABI mismatch");
    }
    if (kind == ShaderKind::sort_anchors_bitonic && !scan.has_array_stride(8)) {
        return std::unexpected("SPIR-V sort_anchors_bitonic range stride mismatch");
    }
    if (kind == ShaderKind::generate_units
        && (!scan.has_binding(0) || !scan.has_binding(1)
            || !scan.has_array_stride(key.max_span / 8u))) {
        return std::unexpected("SPIR-V generate_units layout mismatch");
    }
    if (kind == ShaderKind::phase_c_persisted_directory_select
        && (!scan.has_binding(0) || !scan.has_binding(1)
            || !scan.has_binding(2) || !scan.has_binding(3))) {
        return std::unexpected(
            "SPIR-V persisted directory selection layout mismatch");
    }
    const auto binding_scalar = [&scan](std::uint32_t binding,
                                        std::uint32_t stride) {
        return scan.has_binding_array_stride(0u, binding, stride);
    };
    const auto binding_struct = [&scan](
            std::uint32_t binding,
            std::uint32_t stride,
            std::initializer_list<std::uint32_t> offsets) {
        return scan.has_binding_array_stride(0u, binding, stride)
            && scan.has_binding_struct_offsets(
                0u, binding,
                std::span<const std::uint32_t>{offsets.begin(), offsets.size()});
    };
    const auto core_push = [&scan]() {
        constexpr std::array<std::uint32_t, 18> offsets{
            0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u,
            36u, 40u, 44u, 48u, 52u, 56u, 60u, 64u, 68u};
        return scan.has_push_constant_offsets(offsets);
    };
    const auto partition = [&]() {
        return binding_struct(0u, 64u,
            {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 36u,
             40u, 48u, 56u, 60u});
    };
    const auto control = [&]() {
        return binding_struct(1u, 12u, {0u, 4u, 8u});
    };
    const auto query_record = [&]() {
        return binding_struct(3u, 48u,
            {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 40u, 44u});
    };
    const auto query_minimizer = [&]() {
        return binding_struct(4u, 16u, {0u, 4u, 8u, 12u});
    };
    const auto occurrence = [&]() {
        return binding_struct(5u, 16u, {0u, 4u, 8u, 12u});
    };
    const auto query_state = [&]() {
        return binding_struct(6u, 16u, {0u, 4u, 8u, 12u});
    };
    const auto validation = [&]() {
        return binding_struct(8u, 32u,
            {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u});
    };
    const auto adapter_state_core = [&]() {
        return binding_struct(12u, 32u,
            {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u});
    };
    const auto complete_core_layout = [&]() {
        return partition() && control() && binding_scalar(2u, 4u)
            && query_record() && query_minimizer() && occurrence()
            && query_state()
            && binding_struct(7u, 24u, {0u, 8u, 16u, 20u})
            && validation()
            && binding_struct(9u, 12u, {0u, 4u, 8u})
            && binding_struct(10u, 16u, {0u, 8u, 12u})
            && binding_struct(11u, 24u, {0u, 4u, 8u, 12u, 16u})
            && adapter_state_core() && core_push();
    };
    const bool grouped32_core_kind =
        kind == ShaderKind::phase_c_grouped32_validate
        || kind == ShaderKind::phase_c_grouped32_accumulate_occurrences
        || kind == ShaderKind::phase_c_grouped32_finalize_max_occ
        || kind == ShaderKind::phase_c_grouped32_allocate_anchor_prefix
        || kind == ShaderKind::phase_c_grouped32_build_sort_segments
        || kind == ShaderKind::phase_c_grouped32_emit_global_anchors
        || kind == ShaderKind::phase_c_grouped32_clear_state
        || kind == ShaderKind::phase_c_grouped32_finalize_validation;
    if (grouped32_core_kind && !complete_core_layout()) {
        return std::unexpected(
            "SPIR-V grouped32 ABI2 complete core layout mismatch");
    }
    if (kind == ShaderKind::phase_c_grouped32_validate
        && (!partition() || !control() || !binding_scalar(2u, 4u)
            || !query_record() || !query_minimizer() || !validation()
            || !binding_struct(9u, 12u, {0u, 4u, 8u})
            || !adapter_state_core() || !core_push())) {
        return std::unexpected(
            "SPIR-V grouped32 ABI2 validation layout mismatch");
    }
    if (kind == ShaderKind::phase_c_grouped32_accumulate_occurrences
        && (!partition() || !control() || !query_record()
            || !query_minimizer() || !occurrence() || !validation()
            || !adapter_state_core() || !core_push())) {
        return std::unexpected(
            "SPIR-V grouped32 ABI2 occurrence layout mismatch");
    }
    if (kind == ShaderKind::phase_c_grouped32_finalize_max_occ
        && (!query_record() || !query_minimizer() || !occurrence()
            || !query_state() || !validation() || !adapter_state_core()
            || !core_push())) {
        return std::unexpected(
            "SPIR-V grouped32 ABI2 finalize layout mismatch");
    }
    if (kind == ShaderKind::phase_c_grouped32_allocate_anchor_prefix
        && (!query_record() || !occurrence() || !query_state()
            || !validation() || !adapter_state_core() || !core_push())) {
        return std::unexpected(
            "SPIR-V grouped32 ABI2 prefix layout mismatch");
    }
    if (kind == ShaderKind::phase_c_grouped32_build_sort_segments
        && (!query_record() || !query_state() || !validation()
            || !binding_struct(11u, 24u, {0u, 4u, 8u, 12u, 16u})
            || !adapter_state_core() || !core_push())) {
        return std::unexpected(
            "SPIR-V grouped32 ABI2 sort-ownership layout mismatch");
    }
    if (kind == ShaderKind::phase_c_grouped32_emit_global_anchors
        && (!partition() || !control() || !binding_scalar(2u, 4u)
            || !query_record() || !query_minimizer() || !occurrence()
            || !binding_struct(7u, 24u, {0u, 8u, 16u, 20u})
            || !validation()
            || !binding_struct(10u, 16u, {0u, 8u, 12u})
            || !adapter_state_core() || !core_push())) {
        return std::unexpected(
            "SPIR-V grouped32 ABI2 anchor-emission layout mismatch");
    }
    if (kind == ShaderKind::phase_c_grouped32_adapt_query_sketch
        || kind == ShaderKind::phase_c_grouped32_finalize_query_adapter) {
        constexpr std::array<std::uint32_t, 16> push_offsets{
            0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u,
            32u, 36u, 40u, 44u, 48u, 52u, 56u, 60u};
        const bool ok = binding_scalar(0u, 4u)
            && binding_scalar(1u, 4u) && binding_scalar(2u, 4u)
            && binding_struct(3u, 48u,
                {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 40u})
            && binding_struct(4u, 48u,
                {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 40u, 44u})
            && binding_struct(5u, 16u, {0u, 4u, 8u, 12u})
            && binding_struct(6u, 32u,
                {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u})
            && binding_struct(7u, 32u,
                {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u})
            && binding_struct(8u, 12u, {0u, 4u, 8u})
            && scan.has_push_constant_offsets(push_offsets);
        if (!ok) return std::unexpected(
            "SPIR-V grouped32 ABI2 sketch-adapter layout mismatch");
    }
    if (kind == ShaderKind::phase_c_grouped32_prepare_indirect) {
        constexpr std::array<std::uint32_t, 12> push_offsets{
            0u, 4u, 8u, 12u, 16u, 20u,
            24u, 28u, 32u, 36u, 40u, 44u};
        const bool ok = partition() && query_record() && validation()
            && binding_struct(9u, 12u, {0u, 4u, 8u})
            && adapter_state_core()
            && scan.has_push_constant_offsets(push_offsets);
        if (!ok) return std::unexpected(
            "SPIR-V grouped32 ABI2 indirect-preparation layout mismatch");
    }
    if (kind == ShaderKind::align_extend_banded_affine
        && (!scan.has_binding(0) || !scan.has_binding(1)
            || !scan.has_binding(2) || !scan.has_binding(3)
            || !scan.has_array_stride(4) || !scan.has_array_stride(16)
            || !scan.has_array_stride(32))) {
        return std::unexpected("SPIR-V alignment extension layout mismatch");
    }
    if (kind == ShaderKind::align_extend_simple_cigar
        && (!scan.has_binding(0) || !scan.has_binding(1)
            || !scan.has_binding(2) || !scan.has_binding(3)
            || !scan.has_binding(4) || !scan.has_binding(5)
            || !scan.has_array_stride(4) || !scan.has_array_stride(40)
            || !scan.has_array_stride(48))) {
        return std::unexpected(
            "SPIR-V alignment CIGAR extension layout mismatch");
    }
    return {};
}

struct MslCompileResult {
    std::string msl;
    std::string entry_name;
    std::uint32_t wg_x = 1;
    std::uint32_t wg_y = 1;
    std::uint32_t wg_z = 1;
    bool needs_buffer_size_buffer = false;
    std::uint32_t buffer_size_buffer_index = 25u;
};

[[nodiscard]] inline std::expected<MslCompileResult, std::string> spirv_to_msl(
    std::vector<std::uint32_t> spirv,
    const ShaderVariantKey& key,
    MslVersion version)
{
    try {
        spirv_cross::CompilerMSL compiler(std::move(spirv));
        spirv_cross::CompilerMSL::Options options{};
        options.set_msl_version(version.major, version.minor, version.patch);
        options.argument_buffers = false;
        // MoltenVK and SPIRV-Cross reserve Metal buffer 25 for the runtime
        // array-length table used by OpArrayLength. Keep it explicit and reject
        // any descriptor mapping that would collide with that implicit input.
        options.buffer_size_buffer_index = 25u;
        compiler.set_msl_options(options);

        const ShaderKind kind = checked_shader_kind(key.kind);
        const std::uint32_t push_size = push_constant_size(kind);
        const std::uint32_t binding_count =
            descriptor_binding_count(descriptor_layout_for(kind));
        const std::uint32_t base = push_size > 0 ? 1u : 0u;
        if (base + binding_count > options.buffer_size_buffer_index) {
            return std::unexpected(
                "SPIR-V descriptor mapping collides with MSL buffer-size input");
        }
        const auto resources = compiler.get_shader_resources();
        std::vector<std::uint64_t> observed_storage_bindings;
        observed_storage_bindings.reserve(resources.storage_buffers.size());
        for (const auto& resource : resources.storage_buffers) {
            if (!compiler.has_decoration(
                    resource.id, spv::DecorationDescriptorSet)
                || !compiler.has_decoration(
                    resource.id, spv::DecorationBinding)) {
                return std::unexpected(
                    "SPIR-V storage resource lacks set/binding decoration");
            }
            const std::uint32_t descriptor_set = compiler.get_decoration(
                resource.id, spv::DecorationDescriptorSet);
            const std::uint32_t binding = compiler.get_decoration(
                resource.id, spv::DecorationBinding);
            if (descriptor_set != 0u || binding >= binding_count) {
                return std::unexpected(shader_cache_message(
                    "SPIR-V resource outside descriptor ABI: set=",
                    descriptor_set, " binding=", binding,
                    " binding_count=", binding_count));
            }
            const std::uint64_t identity =
                (std::uint64_t(descriptor_set) << 32u) | binding;
            if (std::find(observed_storage_bindings.begin(),
                          observed_storage_bindings.end(), identity)
                != observed_storage_bindings.end()) {
                return std::unexpected(shader_cache_message(
                    "SPIR-V duplicate storage binding: set=",
                    descriptor_set, " binding=", binding));
            }
            observed_storage_bindings.push_back(identity);
        }
        const std::size_t expected_push_count = push_size == 0u ? 0u : 1u;
        if (resources.push_constant_buffers.size() != expected_push_count) {
            return std::unexpected(shader_cache_message(
                "SPIR-V push-constant resource count mismatch: observed=",
                resources.push_constant_buffers.size(), " expected=",
                expected_push_count));
        }
        auto add_storage_binding = [&compiler](
                std::uint32_t desc_set,
                std::uint32_t binding,
                std::uint32_t count,
                std::uint32_t msl_buffer) {
            spirv_cross::MSLResourceBinding rb{};
            rb.stage = spv::ExecutionModelGLCompute;
            rb.basetype = spirv_cross::SPIRType::Unknown;
            rb.desc_set = desc_set;
            rb.binding = binding;
            rb.count = count;
            rb.msl_buffer = msl_buffer;
            compiler.add_msl_resource_binding(rb);
        };
        if (push_size > 0) {
            add_storage_binding(
                spirv_cross::ResourceBindingPushConstantDescriptorSet,
                spirv_cross::ResourceBindingPushConstantBinding, 1u, 0u);
        }
        for (std::uint32_t binding = 0; binding < binding_count; ++binding) {
            add_storage_binding(0u, binding, 1u, base + binding);
        }

        MslCompileResult result{};
        result.msl = compiler.compile();
        result.entry_name =
            compiler.get_cleansed_entry_point_name("main", spv::ExecutionModelGLCompute);
        result.wg_x = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0);
        result.wg_y = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1);
        result.wg_z = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2);
        result.needs_buffer_size_buffer = compiler.needs_buffer_size_buffer();
        result.buffer_size_buffer_index = options.buffer_size_buffer_index;
        const ShaderWorkgroupSize expected = expected_shader_workgroup_size(key);
        if (result.wg_x != expected.x || result.wg_y != expected.y
            || result.wg_z != expected.z) {
            return std::unexpected(shader_cache_message(
                "SPIR-V/MSL workgroup mismatch: reflected=", result.wg_x,
                'x', result.wg_y, 'x', result.wg_z, " expected=",
                expected.x, 'x', expected.y, 'x', expected.z));
        }
        return result;
    } catch (const std::exception& e) {
        return std::unexpected(shader_cache_message("SPIR-V to MSL failed: ", e.what()));
    }
}

[[nodiscard]] inline std::vector<std::byte> meta_to_bytes(
    const ShaderCacheMeta& meta,
    std::string_view entry_name)
{
    std::vector<std::byte> bytes(sizeof(ShaderCacheMeta) + entry_name.size());
    std::memcpy(bytes.data(), &meta, sizeof(meta));
    std::memcpy(bytes.data() + sizeof(meta), entry_name.data(), entry_name.size());
    return bytes;
}

[[nodiscard]] inline std::expected<ShaderCacheMeta, std::string> read_meta(
    const std::filesystem::path& path,
    std::string* entry_name)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected("meta missing");
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size < static_cast<std::streamoff>(sizeof(ShaderCacheMeta))) {
        return std::unexpected("meta too small");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) {
        return std::unexpected("meta read failed");
    }
    ShaderCacheMeta meta{};
    std::memcpy(&meta, bytes.data(), sizeof(meta));
    if (meta.magic != 0xB10C0CACu || meta.version != 1) {
        return std::unexpected("meta magic/version mismatch");
    }
    if (meta.entry_len > 64u
        || sizeof(ShaderCacheMeta) + meta.entry_len > bytes.size()) {
        return std::unexpected("meta entry length invalid");
    }
    if (entry_name != nullptr) {
        entry_name->assign(
            reinterpret_cast<const char*>(bytes.data() + sizeof(ShaderCacheMeta)),
            meta.entry_len);
    }
    return meta;
}

[[nodiscard]] inline std::expected<ShaderCacheMeta, std::string>
read_compatible_cache_meta(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key,
    std::string* entry_name)
{
    std::string entry;
    auto meta = read_meta(cache_meta_path(cache_dir, key), &entry);
    if (!meta) {
        return std::unexpected(meta.error());
    }
    if (!(meta->key == key)) {
        return std::unexpected("meta key mismatch");
    }
    if (entry.empty()) {
        return std::unexpected("meta entry name is empty");
    }
    const ShaderWorkgroupSize expected = expected_shader_workgroup_size(key);
    if (meta->wg_x != expected.x
        || meta->wg_y != expected.y
        || meta->wg_z != expected.z) {
        return std::unexpected(shader_cache_message(
            "meta workgroup mismatch: cached=", meta->wg_x, 'x',
            meta->wg_y, 'x', meta->wg_z, " expected=", expected.x, 'x',
            expected.y, 'x', expected.z));
    }
    if (entry_name != nullptr) {
        *entry_name = entry;
    }
    return *meta;
}

[[nodiscard]] inline std::expected<void, std::string> require_nonempty_cache_file(
    const std::filesystem::path& path,
    std::string_view artifact)
{
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        return std::unexpected(shader_cache_message(
            artifact, " stat failed for ", path.string(), ": ", ec.message()));
    }
    if (!exists) {
        return std::unexpected(shader_cache_message(
            artifact, " missing: ", path.string()));
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(shader_cache_message(
            artifact, " size failed for ", path.string(), ": ", ec.message()));
    }
    if (size == 0) {
        return std::unexpected(shader_cache_message(
            artifact, " empty: ", path.string()));
    }
    return {};
}

[[nodiscard]] inline std::expected<void, std::string>
cache_metallib_entry_status(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key)
{
    if (auto meta = read_compatible_cache_meta(cache_dir, key, nullptr); !meta) {
        return std::unexpected(shader_cache_message(
            "metadata incompatible: ", meta.error()));
    }
    return require_nonempty_cache_file(
        cache_metallib_path(cache_dir, key), "metallib");
}

[[nodiscard]] inline bool cache_metallib_entry_complete(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key)
{
    return cache_metallib_entry_status(cache_dir, key).has_value();
}

[[nodiscard]] inline std::expected<void, std::string>
cache_binary_archive_status(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key)
{
#if !defined(__APPLE__)
    (void)cache_dir;
    (void)key;
    return {};
#else
    for (const std::filesystem::path& archive : cache_binary_archive_paths(cache_dir, key)) {
        if (auto present = require_nonempty_cache_file(archive, "Metal binary archive");
            !present) {
            return present;
        }
    }
    return {};
#endif
}

[[nodiscard]] inline bool cache_binary_archive_complete(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key)
{
    return cache_binary_archive_status(cache_dir, key).has_value();
}

[[nodiscard]] inline std::expected<void, std::string> cache_entry_status(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key)
{
    if (auto metallib = cache_metallib_entry_status(cache_dir, key); !metallib) {
        return metallib;
    }
    return cache_binary_archive_status(cache_dir, key);
}

[[nodiscard]] inline bool cache_entry_complete(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key)
{
    return cache_entry_status(cache_dir, key).has_value();
}

[[nodiscard]] inline std::expected<std::filesystem::path, std::string>
metal_archive_helper_path()
{
#if !defined(__APPLE__)
    return std::filesystem::path{};
#else
    auto executable_candidate = [](const std::filesystem::path& path)
        -> std::optional<std::filesystem::path> {
        if (!path.empty() && ::access(path.c_str(), X_OK) == 0) {
            return path;
        }
        return std::nullopt;
    };

    if (const char* env = std::getenv("BIOSEQ_GPU_ARCHIVE_HELPER");
        env != nullptr && *env != '\0') {
        if (auto path = executable_candidate(env)) {
            return *path;
        }
        return std::unexpected(shader_cache_message(
            "BIOSEQ_GPU_ARCHIVE_HELPER is not executable: ", env));
    }

    Dl_info info{};
    if (::dladdr(reinterpret_cast<const void*>(&metal_archive_helper_path), &info) != 0
        && info.dli_fname != nullptr) {
        const std::filesystem::path dylib_path(info.dli_fname);
        if (auto path = executable_candidate(
                dylib_path.parent_path() / "bioseq_gpu_archive_builder")) {
            return *path;
        }
    }

    return std::unexpected(
        "bioseq_gpu_archive_builder not found next to bioseq.dylib; set BIOSEQ_GPU_ARCHIVE_HELPER");
#endif
}

[[nodiscard]] inline std::expected<void, std::string> ensure_binary_archive(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key,
    const ShaderCacheMeta& meta,
    std::string_view entry_name)
{
#if !defined(__APPLE__)
    (void)cache_dir;
    (void)key;
    (void)meta;
    (void)entry_name;
    return {};
#else
    if (entry_name.empty()) {
        return std::unexpected(shader_cache_message(
            "cannot build MTLBinaryArchive for ", cache_file_stem(key),
            ": empty Metal entry name"));
    }
    const auto helper = metal_archive_helper_path();
    if (!helper) {
        return std::unexpected(helper.error());
    }

    const std::uint64_t shader_threads =
        static_cast<std::uint64_t>(std::max<std::uint32_t>(meta.wg_x, 1u))
        * static_cast<std::uint64_t>(std::max<std::uint32_t>(meta.wg_y, 1u))
        * static_cast<std::uint64_t>(std::max<std::uint32_t>(meta.wg_z, 1u));
    // MoltenVK may create the Metal pipeline descriptor with the conservative
    // device cap rather than the GLSL local size. Keep both archives so a
    // forked backend can match the descriptor without invoking MTLCompilerService.
    const std::vector<std::uint64_t> archive_thread_caps = [](
            std::uint64_t exact_threads) {
        constexpr std::uint64_t conservative_moltenvk_cap = 256u;
        std::vector<std::uint64_t> caps{exact_threads};
        if (exact_threads != conservative_moltenvk_cap) {
            caps.push_back(conservative_moltenvk_cap);
        }
        return caps;
    }(shader_threads);

    std::error_code ec;
    for (std::size_t cap_index = 0; cap_index < archive_thread_caps.size(); ++cap_index) {
        const std::uint64_t max_threads = archive_thread_caps[cap_index];
        const std::filesystem::path final_path = cap_index == 0
            ? cache_binary_archive_path(cache_dir, key)
            : cache_dir / (cache_file_stem(key) + ".max_threads_"
                           + std::to_string(max_threads) + ".metalar");
        if (std::filesystem::exists(final_path, ec) && !ec
            && std::filesystem::file_size(final_path, ec) > 0 && !ec) {
            continue;
        }
        ec.clear();
        const std::filesystem::path tmp_path = final_path.string() + ".tmp";
        std::filesystem::remove(tmp_path, ec);
        ec.clear();

        const std::vector<std::string> args{
            helper->string(),
            cache_metallib_path(cache_dir, key).string(),
            tmp_path.string(),
            std::string(entry_name),
            std::to_string(max_threads),
            "0",
        };
        auto result = run_capture(args);
        if (!result) {
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper failed for ", cache_file_stem(key), ": ",
                result.error()));
        }
        if (!std::filesystem::exists(tmp_path, ec) || ec
            || std::filesystem::file_size(tmp_path, ec) == 0 || ec) {
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper produced no archive for ",
                cache_file_stem(key), ": ", *result));
        }
        if (auto renamed = durable_rename_existing(tmp_path, final_path); !renamed) {
            return renamed;
        }
    }
    return {};
#endif
}

[[nodiscard]] inline constexpr std::size_t moltenvk_msl_spirv_header_size() noexcept
{
    return sizeof(MVKMSLSPIRVHeader);
}

[[nodiscard]] inline constexpr std::uint32_t moltenvk_msl_compiled_code_magic() noexcept
{
    return static_cast<std::uint32_t>(kMVKMagicNumberMSLCompiledCode);
}

[[nodiscard]] inline std::expected<MoltenVkShaderModuleCode, std::string>
make_moltenvk_metallib_module_code(const std::filesystem::path& metallib_path)
{
    std::ifstream input(metallib_path, std::ios::binary);
    if (!input) {
        return std::unexpected("metallib missing");
    }
    input.seekg(0, std::ios::end);
    const std::streamoff metallib_size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (metallib_size <= 0) {
        return std::unexpected("metallib empty");
    }

    MoltenVkShaderModuleCode code{};
    code.header_size_bytes = sizeof(MVKMSLSPIRVHeader);
    code.code_size_bytes =
        code.header_size_bytes + static_cast<std::size_t>(metallib_size);
    code.words.resize((code.code_size_bytes + sizeof(std::uint32_t) - 1u)
                      / sizeof(std::uint32_t));

    auto* bytes = reinterpret_cast<std::byte*>(code.words.data());
    const MVKMSLSPIRVHeader header =
        static_cast<MVKMSLSPIRVHeader>(kMVKMagicNumberMSLCompiledCode);
    std::memcpy(bytes, &header, sizeof(header));
    input.read(
        reinterpret_cast<char*>(bytes + code.header_size_bytes),
        metallib_size);
    if (!input) {
        return std::unexpected("metallib read failed");
    }
    return code;
}

[[nodiscard]] inline std::expected<void, std::string> compile_variant_to_cache(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key,
    MslVersion msl_version)
{
    if (!valid_shader_variant_key(key)) {
        return std::unexpected(shader_cache_message(
            "invalid shader variant ", cache_file_stem(key)));
    }
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        return std::unexpected(shader_cache_message(
            "create cache dir ", cache_dir.string(), " failed: ", ec.message()));
    }
    if (cache_entry_complete(cache_dir, key)) {
        return {};
    }
    if (cache_metallib_entry_complete(cache_dir, key)) {
        std::string existing_entry;
        auto existing_meta = read_meta(cache_meta_path(cache_dir, key), &existing_entry);
        if (!existing_meta) {
            return std::unexpected(existing_meta.error());
        }
        return ensure_binary_archive(cache_dir, key, *existing_meta, existing_entry);
    }

    std::vector<std::uint32_t> spirv;
    try {
        spirv = compile_shader_spirv(key.to_request());
    } catch (const std::exception& e) {
        return std::unexpected(shader_cache_message("glslang failed: ", e.what()));
    }
    if (auto valid = validate_spirv_layout(key, spirv); !valid) {
        return valid;
    }
    auto msl = spirv_to_msl(std::move(spirv), key, msl_version);
    if (!msl) {
        return std::unexpected(msl.error());
    }
    const ShaderWorkgroupSize expected_workgroup = expected_shader_workgroup_size(key);
    const ShaderWorkgroupSize compiled_workgroup{
        msl->wg_x == 0 ? 1u : msl->wg_x,
        msl->wg_y == 0 ? 1u : msl->wg_y,
        msl->wg_z == 0 ? 1u : msl->wg_z};
    if (!(compiled_workgroup == expected_workgroup)) {
        return std::unexpected(shader_cache_message(
            "compiled SPIR-V workgroup mismatch for ", cache_file_stem(key),
            ": reflected=", compiled_workgroup.x, 'x', compiled_workgroup.y, 'x',
            compiled_workgroup.z, " expected=", expected_workgroup.x, 'x',
            expected_workgroup.y, 'x', expected_workgroup.z));
    }

    const std::filesystem::path stem = cache_dir / cache_file_stem(key);
    const std::filesystem::path metal_path = stem.string() + ".tmp.metal";
    const std::filesystem::path air_path = stem.string() + ".air.tmp";
    const std::filesystem::path metallib_tmp = stem.string() + ".metallib.tmp";
    const std::filesystem::path metallib_final = cache_metallib_path(cache_dir, key);
    const std::span<const std::byte> msl_bytes{
        reinterpret_cast<const std::byte*>(msl->msl.data()), msl->msl.size()};
    if (auto written = durable_write_file(metal_path, msl_bytes); !written) {
        return written;
    }

    const std::string metal_std = shader_cache_message(
        "metal", msl_version.major, '.', msl_version.minor);
    const std::vector<std::string> metal_args{
        xcrun_executable(), "-sdk", "macosx", "metal", "-std=" + metal_std,
        "-c", metal_path.string(), "-o", air_path.string()};
    if (auto compiled = spawn_wait(metal_args); !compiled) {
        return compiled;
    }
    const std::vector<std::string> metallib_args{
        xcrun_executable(), "-sdk", "macosx", "metallib",
        air_path.string(), "-o", metallib_tmp.string()};
    if (auto linked = spawn_wait(metallib_args); !linked) {
        return linked;
    }
    if (auto renamed = durable_rename_existing(metallib_tmp, metallib_final); !renamed) {
        return renamed;
    }

    ShaderCacheMeta meta{};
    meta.key = key;
    meta.wg_x = compiled_workgroup.x;
    meta.wg_y = compiled_workgroup.y;
    meta.wg_z = compiled_workgroup.z;
    meta.entry_len = static_cast<std::uint32_t>(
        std::min<std::size_t>(msl->entry_name.size(), 64u));
    const std::string entry = msl->entry_name.substr(0, meta.entry_len);
    const std::vector<std::byte> meta_bytes = meta_to_bytes(meta, entry);
    if (auto meta_written = durable_write_file(cache_meta_path(cache_dir, key), meta_bytes);
        !meta_written) {
        return meta_written;
    }
    if (auto archive_written = ensure_binary_archive(cache_dir, key, meta, entry);
        !archive_written) {
        return archive_written;
    }
    std::filesystem::remove(metal_path, ec);
    std::filesystem::remove(air_path, ec);
    return {};
}

[[nodiscard]] inline std::expected<void, std::string> ensure_moltenvk_command_library(
    const std::filesystem::path& cache_dir,
    MslVersion msl_version)
{
#if !defined(__APPLE__)
    (void)cache_dir;
    (void)msl_version;
    return {};
#else
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        return std::unexpected(shader_cache_message(
            "create cache dir ", cache_dir.string(), " failed: ", ec.message()));
    }
    const std::filesystem::path final_path = moltenvk_command_metallib_path(cache_dir);
    if (std::filesystem::exists(final_path, ec) && !ec) {
        return {};
    }

    const std::filesystem::path stem = cache_dir / "moltenvk_command";
    const std::filesystem::path metal_path = stem.string() + ".tmp.metal";
    const std::filesystem::path air_path = stem.string() + ".air.tmp";
    const std::filesystem::path tmp_path = stem.string() + ".metallib.tmp";
    const std::string_view msl = moltenvk_command_placeholder_msl();
    const std::span<const std::byte> msl_bytes{
        reinterpret_cast<const std::byte*>(msl.data()), msl.size()};
    if (auto written = durable_write_file(metal_path, msl_bytes); !written) {
        return written;
    }

    const std::string metal_std = shader_cache_message(
        "metal", msl_version.major, '.', msl_version.minor);
    const std::vector<std::string> metal_args{
        xcrun_executable(), "-sdk", "macosx", "metal", "-std=" + metal_std,
        "-c", metal_path.string(), "-o", air_path.string()};
    if (auto compiled = spawn_wait(metal_args); !compiled) {
        return compiled;
    }
    const std::vector<std::string> metallib_args{
        xcrun_executable(), "-sdk", "macosx", "metallib",
        air_path.string(), "-o", tmp_path.string()};
    if (auto linked = spawn_wait(metallib_args); !linked) {
        return linked;
    }
    if (auto renamed = durable_rename_existing(tmp_path, final_path); !renamed) {
        return renamed;
    }
    std::filesystem::remove(metal_path, ec);
    std::filesystem::remove(air_path, ec);
    return fsync_parent_dir(final_path);
#endif
}

[[nodiscard]] inline std::expected<void, std::string> ensure_compiled(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key,
    MslVersion version)
{
    if (cache_entry_complete(cache_dir, key)) {
        return {};
    }
    return compile_variant_to_cache(cache_dir, key, version);
}

[[nodiscard]] inline std::expected<ShaderModuleLoad, std::string> load_cached_module(
    VkDevice device,
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key,
    PFN_vkCreateShaderModule create_shader_module)
{
    if (create_shader_module == nullptr) {
        return std::unexpected("vkCreateShaderModule function pointer is null");
    }
    auto code = make_moltenvk_metallib_module_code(cache_metallib_path(cache_dir, key));
    if (!code) {
        return std::unexpected(code.error());
    }

    // Re-read and validate metadata after the metallib bytes are resident and
    // immediately before module creation.  The earlier completeness probe is
    // only an optimization; it cannot be the trust boundary because a compile
    // service may report success without publishing a compatible record, and
    // cache files may be replaced between the probe and this loader.
    std::string entry;
    auto meta = read_compatible_cache_meta(cache_dir, key, &entry);
    if (!meta) {
        return std::unexpected(meta.error());
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = code->code_size_bytes;
    module_info.pCode = code->words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult vk_result =
        create_shader_module(device, &module_info, nullptr, &module);
    if (vk_result != VK_SUCCESS) {
        return std::unexpected(shader_cache_message(
            "vkCreateShaderModule(metallib): VkResult ",
            static_cast<int>(vk_result)));
    }
    return ShaderModuleLoad{module, entry, meta->wg_x, meta->wg_y, meta->wg_z};
}

template <is_runtime_compile_service Service>
[[nodiscard]] inline std::expected<ShaderModuleLoad, std::string> load_module(
    VkDevice device,
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key,
    PFN_vkCreateShaderModule create_shader_module,
    Service& service)
{
    if (!cache_entry_complete(cache_dir, key)) {
        auto compile = service.ensure_variant_compiled(key);
        if (!compile) {
            return std::unexpected(compile.error());
        }
        if (auto published = cache_entry_status(cache_dir, key); !published) {
            return std::unexpected(shader_cache_message(
                "shader compile service returned success without a complete compatible cache entry for ",
                cache_file_stem(key), ": ", published.error()));
        }
    }
    return load_cached_module(device, cache_dir, key, create_shader_module);
}

} // namespace bioseq::gpu::vulkan

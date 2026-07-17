/**
 * @file shader_cache.cppm
 * @brief Supervisor-built Vulkan shader metallib cache for MoltenVK backends.
 */
module;
#include "common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
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
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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
import bioseq.parallelism.memory_admission;

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
static_assert(std::is_nothrow_move_constructible_v<ShaderModuleLoad>);

struct MoltenVkShaderModuleCode {
    // Declared first so words release before the transient permit.
    bioseq::parallelism::MemoryPermit permit;
    std::vector<std::uint32_t> words;
    std::size_t code_size_bytes = 0;
    std::size_t header_size_bytes = sizeof(MVKMSLSPIRVHeader);
    std::uint64_t admission_count = 0u;
    std::uint64_t peak_admitted_bytes = 0u;
    std::uint64_t retained_capacity_bytes = 0u;
    bool admission_saturated = false;
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

enum class SealedShaderCacheArtifactKind : std::uint32_t {
    // Wire value 0 was used by an abandoned placeholder command library.
    // It remains reserved so v1 manifests cannot be reinterpreted as a
    // different artifact kind. Exact MoltenVK command sources are not yet an
    // immutable AOT artifact and therefore never appear in a sealed seed.
    reserved_command_metallib = 0,
    shader_meta = 1,
    shader_metallib = 2,
    metal_binary_archive = 3,
    metal_binary_archive_qualification = 4,
};

struct SealedShaderCacheArtifact {
    SealedShaderCacheArtifactKind kind =
        SealedShaderCacheArtifactKind::reserved_command_metallib;
    std::uint32_t key_index = (std::numeric_limits<std::uint32_t>::max)();
    std::string filename;
    std::uint64_t size = 0;
    std::array<std::uint8_t, 32> sha256{};

    [[nodiscard]] bool operator==(
        const SealedShaderCacheArtifact&) const noexcept = default;
};

struct SealedShaderCacheManifest {
    std::string cache_digest;
    std::vector<ShaderVariantKey> keys;
    std::vector<SealedShaderCacheArtifact> artifacts;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0;
    std::uint64_t manifest_checksum = 0;
};

struct MetalArchiveNegativeErrorFingerprint {
    std::string domain;
    std::int64_t code = 0;

    [[nodiscard]] bool accepted() const noexcept {
        return !domain.empty() && domain.size() <= 1024u
            && domain.find('\0') == std::string::npos;
    }
    [[nodiscard]] bool operator==(
        const MetalArchiveNegativeErrorFingerprint&) const noexcept = default;
};

struct MetalArchiveQualificationReceipt {
    std::uint32_t wire_version = 0u;
    ShaderVariantKey shader_key{};
    std::uint64_t metallib_size = 0u;
    std::array<std::uint8_t, 32> metallib_sha256{};
    std::string entry_name;
    std::uint64_t max_total_threads_per_threadgroup = 0u;
    bool threadgroup_size_is_multiple = false;
    MetalArchiveNegativeErrorFingerprint negative_error;
    std::uint64_t archive_size = 0u;
    std::array<std::uint8_t, 32> archive_sha256{};
    std::uint32_t helper_protocol_version = 0u;
    std::string helper_build_identity;
    std::string helper_source;
    std::uint64_t helper_executable_size = 0u;
    std::array<std::uint8_t, 32> helper_executable_sha256{};
    std::uint64_t helper_metal_registry_id = 0u;

    [[nodiscard]] bool diagnostic_valid() const noexcept {
        return (wire_version == 1u || wire_version == 2u)
            && max_total_threads_per_threadgroup > 0u
            && max_total_threads_per_threadgroup <= 1024u
            && negative_error.accepted() && archive_size != 0u
            && std::any_of(
                archive_sha256.begin(), archive_sha256.end(),
                [](std::uint8_t byte) { return byte != 0u; });
    }
    [[nodiscard]] bool accepted() const noexcept {
        const auto nonzero_digest = [](const auto& digest) {
            return std::any_of(
                digest.begin(), digest.end(),
                [](std::uint8_t byte) { return byte != 0u; });
        };
        return diagnostic_valid() && wire_version == 2u
            && valid_shader_variant_key(shader_key)
            && metallib_size != 0u && nonzero_digest(metallib_sha256)
            && !entry_name.empty() && entry_name.size() <= 1024u
            && entry_name.find('\0') == std::string::npos
            && helper_protocol_version == 2u
            && !helper_build_identity.empty()
            && helper_build_identity.size() <= 1024u
            && helper_build_identity.find('\0') == std::string::npos
            && !helper_source.empty() && helper_source.size() <= 64u
            && helper_source.find('\0') == std::string::npos
            && helper_executable_size != 0u
            && nonzero_digest(helper_executable_sha256)
            && helper_metal_registry_id != 0u;
    }
    [[nodiscard]] bool production_accepted() const noexcept {
        return accepted() && helper_source == "installed-sibling"
            && helper_build_identity == "bioseq_gpu_archive_builder_v2";
    }
    [[nodiscard]] bool operator==(
        const MetalArchiveQualificationReceipt&) const noexcept = default;
};

[[nodiscard]] std::expected<void, std::string>
validate_metal_archive_receipt_against_process_helper(
    const MetalArchiveQualificationReceipt& receipt);

/// One exact artifact byte image retained from the single sealed-seed load.
/// The descriptor and bytes are immutable after the private seed constructor
/// publishes the image.
struct SealedShaderCacheArtifactImage {
    SealedShaderCacheArtifact descriptor;
    std::vector<std::byte> bytes;
};

/// Loader-originated evidence for one complete process-local seed image.
/// Construction is private: runtime configuration cannot manufacture the
/// counters or copy them from manifest metadata without executing the loader.
class SealedShaderCacheSeedReceipt {
public:
    [[nodiscard]] std::uint64_t identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] std::uint64_t load_count() const noexcept {
        return load_count_;
    }
    [[nodiscard]] std::uint64_t manifest_read_count() const noexcept {
        return manifest_read_count_;
    }
    [[nodiscard]] std::uint64_t artifact_read_count() const noexcept {
        return artifact_read_count_;
    }
    [[nodiscard]] std::uint64_t manifest_bytes() const noexcept {
        return manifest_bytes_;
    }
    [[nodiscard]] std::uint64_t artifact_bytes() const noexcept {
        return artifact_bytes_;
    }
    [[nodiscard]] std::uint64_t load_ns() const noexcept {
        return load_ns_;
    }
    [[nodiscard]] std::uint64_t admission_count() const noexcept {
        return admission_count_;
    }
    [[nodiscard]] std::uint64_t peak_admitted_bytes() const noexcept {
        return peak_admitted_bytes_;
    }
    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        return retained_capacity_bytes_;
    }
    [[nodiscard]] bool admission_saturated() const noexcept {
        return admission_saturated_;
    }
    [[nodiscard]] bool accepted() const noexcept {
        return identity_ != 0u && load_count_ == 1u
            && manifest_read_count_ == 1u && artifact_read_count_ != 0u
            && manifest_bytes_ != 0u && artifact_bytes_ != 0u
            && load_ns_ != 0u && admission_count_ == 1u
            && peak_admitted_bytes_ != 0u
            && retained_capacity_bytes_ != 0u
            && retained_capacity_bytes_ <= peak_admitted_bytes_
            && !admission_saturated_;
    }

private:
    SealedShaderCacheSeedReceipt(
        std::uint64_t identity,
        std::uint64_t artifact_read_count,
        std::uint64_t manifest_bytes,
        std::uint64_t artifact_bytes,
        std::uint64_t load_ns,
        std::uint64_t peak_admitted_bytes) noexcept
        : identity_(identity), artifact_read_count_(artifact_read_count),
          manifest_bytes_(manifest_bytes), artifact_bytes_(artifact_bytes),
          load_ns_(load_ns), peak_admitted_bytes_(peak_admitted_bytes) {}

    std::uint64_t identity_ = 0u;
    std::uint64_t load_count_ = 1u;
    std::uint64_t manifest_read_count_ = 1u;
    std::uint64_t artifact_read_count_ = 0u;
    std::uint64_t manifest_bytes_ = 0u;
    std::uint64_t artifact_bytes_ = 0u;
    std::uint64_t load_ns_ = 0u;
    std::uint64_t admission_count_ = 1u;
    std::uint64_t peak_admitted_bytes_ = 0u;
    std::uint64_t retained_capacity_bytes_ = 0u;
    bool admission_saturated_ = false;

    friend class SealedShaderCacheSeedImage;
};

/// Complete immutable shader seed retained by one backend process. Every
/// consumer receives spans into these exact verified reads; no consumer needs
/// to reopen a cache pathname after this image is installed.
class SealedShaderCacheSeedImage {
public:
    SealedShaderCacheSeedImage(const SealedShaderCacheSeedImage&) = delete;
    SealedShaderCacheSeedImage& operator=(
        const SealedShaderCacheSeedImage&) = delete;
    SealedShaderCacheSeedImage(SealedShaderCacheSeedImage&&) noexcept = default;
    SealedShaderCacheSeedImage& operator=(
        SealedShaderCacheSeedImage&&) noexcept = delete;

    [[nodiscard]] const SealedShaderCacheManifest& manifest() const noexcept {
        return manifest_;
    }
    [[nodiscard]] std::span<const std::byte> manifest_bytes() const noexcept {
        return manifest_bytes_;
    }
    [[nodiscard]] std::span<const SealedShaderCacheArtifactImage>
    artifacts() const noexcept {
        return artifacts_;
    }
    [[nodiscard]] const SealedShaderCacheSeedReceipt& receipt() const noexcept {
        return receipt_;
    }
    [[nodiscard]] const SealedShaderCacheArtifactImage* artifact(
        std::size_t ordinal) const noexcept {
        return ordinal < artifacts_.size() ? &artifacts_[ordinal] : nullptr;
    }
    [[nodiscard]] bool accepted() const noexcept {
        return receipt_.accepted()
            && artifacts_.size() == manifest_.artifacts.size()
            && receipt_.artifact_read_count() == artifacts_.size()
            && receipt_.artifact_bytes() == manifest_.seed_size
            && receipt_.manifest_bytes() == manifest_bytes_.size()
            && permit_ && permit_.bytes()
                == receipt_.retained_capacity_bytes();
    }
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::parallelism::saturating_add;
        using bioseq::parallelism::saturating_mul;
        std::uint64_t owned = sizeof(SealedShaderCacheSeedImage);
        owned = saturating_add(
            owned, static_cast<std::uint64_t>(manifest_.cache_digest.capacity()));
        owned = saturating_add(
            owned,
            saturating_mul(
                static_cast<std::uint64_t>(manifest_.keys.capacity()),
                sizeof(ShaderVariantKey)));
        owned = saturating_add(
            owned,
            saturating_mul(
                static_cast<std::uint64_t>(manifest_.artifacts.capacity()),
                sizeof(SealedShaderCacheArtifact)));
        for (const SealedShaderCacheArtifact& artifact : manifest_.artifacts) {
            owned = saturating_add(
                owned,
                static_cast<std::uint64_t>(artifact.filename.capacity()));
        }
        owned = saturating_add(
            owned,
            static_cast<std::uint64_t>(manifest_bytes_.capacity()));
        owned = saturating_add(
            owned,
            saturating_mul(
                static_cast<std::uint64_t>(artifacts_.capacity()),
                sizeof(SealedShaderCacheArtifactImage)));
        for (const SealedShaderCacheArtifactImage& artifact : artifacts_) {
            owned = saturating_add(
                owned,
                static_cast<std::uint64_t>(
                    artifact.descriptor.filename.capacity()));
            owned = saturating_add(
                owned, static_cast<std::uint64_t>(artifact.bytes.capacity()));
        }
        return owned;
    }

private:
    SealedShaderCacheSeedImage(
        SealedShaderCacheManifest manifest,
        std::vector<std::byte> manifest_bytes,
        std::vector<SealedShaderCacheArtifactImage> artifacts,
        std::uint64_t receipt_identity,
        std::uint64_t artifact_read_count,
        std::uint64_t loaded_manifest_bytes,
        std::uint64_t loaded_artifact_bytes,
        std::uint64_t load_ns,
        std::uint64_t peak_admitted_bytes,
        bioseq::parallelism::MemoryPermit permit) noexcept
        : permit_(std::move(permit)), manifest_(std::move(manifest)),
          manifest_bytes_(std::move(manifest_bytes)),
          artifacts_(std::move(artifacts)),
          receipt_(
              receipt_identity, artifact_read_count, loaded_manifest_bytes,
              loaded_artifact_bytes, load_ns, peak_admitted_bytes) {}

    [[nodiscard]] bool reconcile_admission() noexcept {
        const std::uint64_t retained = owned_capacity_bytes();
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || !permit_ || retained > permit_.bytes()) {
            receipt_.admission_saturated_ = true;
            return false;
        }
        permit_.shrink_to(retained);
        receipt_.retained_capacity_bytes_ = retained;
        return true;
    }

    // Declared first so reverse member destruction releases every admitted
    // allocation before returning the lifetime permit to the controller.
    bioseq::parallelism::MemoryPermit permit_;
    SealedShaderCacheManifest manifest_;
    std::vector<std::byte> manifest_bytes_;
    std::vector<SealedShaderCacheArtifactImage> artifacts_;
    SealedShaderCacheSeedReceipt receipt_;

    friend std::expected<SealedShaderCacheSeedImage, std::string>
    load_validated_sealed_shader_cache_seed(
        const std::filesystem::path&,
        std::string_view,
        std::span<const ShaderVariantKey>);
};

/// Exact immutable inputs retained from the same reads whose sizes and
/// SHA-256 values were checked against the sealed manifest. Module creation
/// consumes these bytes directly and never reopens a cache pathname.
struct SealedShaderCacheKeyImage {
    SealedShaderCacheManifest manifest;
    ShaderVariantKey key{};
    ShaderCacheMeta meta{};
    std::string entry_name;
    std::vector<std::byte> metallib_bytes;
};

/// Borrowed view into a process-owned seed image. The caller must keep the
/// seed alive until module creation returns.
struct SealedShaderCacheKeyView {
    ShaderVariantKey key{};
    ShaderCacheMeta meta{};
    std::string entry_name;
    std::span<const std::byte> metallib_bytes;
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

[[nodiscard]] inline std::filesystem::path
cache_binary_archive_qualification_path(
    const std::filesystem::path& archive_path)
{
    return archive_path.string() + ".qualification";
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

[[nodiscard]] inline std::filesystem::path sealed_shader_cache_manifest_path(
    const std::filesystem::path& cache_dir)
{
    return cache_dir / "bioseq_shader_cache.sealed.v3";
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

struct RunCaptureLimits {
    std::size_t maximum_output_bytes = 4u << 20u;
    std::chrono::milliseconds timeout = std::chrono::minutes(5);
};

struct RunCaptureInheritedFd {
    int source_fd = -1;
    int target_fd = -1;
};

class CaptureFileDescriptor {
public:
    explicit CaptureFileDescriptor(int fd = -1) noexcept : fd_(fd) {}
    CaptureFileDescriptor(const CaptureFileDescriptor&) = delete;
    CaptureFileDescriptor& operator=(const CaptureFileDescriptor&) = delete;
    ~CaptureFileDescriptor() noexcept { close_now(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
    void close_now() noexcept {
        const int fd = std::exchange(fd_, -1);
        if (fd >= 0) (void)::close(fd);
    }

private:
    int fd_ = -1;
};

class CaptureSpawnFileActions {
public:
    CaptureSpawnFileActions() = default;
    CaptureSpawnFileActions(const CaptureSpawnFileActions&) = delete;
    CaptureSpawnFileActions& operator=(const CaptureSpawnFileActions&) = delete;
    ~CaptureSpawnFileActions() noexcept {
        if (initialized_) (void)::posix_spawn_file_actions_destroy(&actions_);
    }

    [[nodiscard]] int initialize() noexcept {
        const int rc = ::posix_spawn_file_actions_init(&actions_);
        initialized_ = rc == 0;
        return rc;
    }
    [[nodiscard]] int add_dup2(int fd, int target) noexcept {
        return ::posix_spawn_file_actions_adddup2(&actions_, fd, target);
    }
    [[nodiscard]] int add_close(int fd) noexcept {
        return ::posix_spawn_file_actions_addclose(&actions_, fd);
    }
    [[nodiscard]] const posix_spawn_file_actions_t* get() const noexcept {
        return &actions_;
    }

private:
    posix_spawn_file_actions_t actions_{};
    bool initialized_ = false;
};

class CaptureSpawnAttributes {
public:
    CaptureSpawnAttributes() = default;
    CaptureSpawnAttributes(const CaptureSpawnAttributes&) = delete;
    CaptureSpawnAttributes& operator=(const CaptureSpawnAttributes&) = delete;
    ~CaptureSpawnAttributes() noexcept {
        if (initialized_) (void)::posix_spawnattr_destroy(&attributes_);
    }

    [[nodiscard]] int initialize_process_group() noexcept {
        int rc = ::posix_spawnattr_init(&attributes_);
        if (rc != 0) return rc;
        initialized_ = true;
        rc = ::posix_spawnattr_setpgroup(&attributes_, 0);
        if (rc != 0) return rc;
        return ::posix_spawnattr_setflags(
            &attributes_, POSIX_SPAWN_SETPGROUP);
    }
    [[nodiscard]] const posix_spawnattr_t* get() const noexcept {
        return &attributes_;
    }

private:
    posix_spawnattr_t attributes_{};
    bool initialized_ = false;
};

class CapturedChildProcess {
public:
    CapturedChildProcess(int read_fd, pid_t pid) noexcept
        : read_fd_(read_fd), pid_(pid) {}

    CapturedChildProcess(const CapturedChildProcess&) = delete;
    CapturedChildProcess& operator=(const CapturedChildProcess&) = delete;

    ~CapturedChildProcess() noexcept {
        close_read();
        if (!completed_) terminate_group_and_reap();
    }

    [[nodiscard]] int read_fd() const noexcept { return read_fd_; }

    void close_read() noexcept {
        if (read_fd_ >= 0) {
            (void)::close(read_fd_);
            read_fd_ = -1;
        }
    }

    void mark_reaped() noexcept { direct_child_reaped_ = true; }
    void mark_completed() noexcept { completed_ = true; }

private:
    enum class ReapProbe : std::uint8_t {
        running,
        reaped,
        no_longer_a_child,
        error,
    };

    [[nodiscard]] ReapProbe probe_reap() noexcept {
        if (direct_child_reaped_) return ReapProbe::reaped;
        int status = 0;
        for (;;) {
            const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                direct_child_reaped_ = true;
                return ReapProbe::reaped;
            }
            if (waited == 0) return ReapProbe::running;
            if (waited < 0 && errno == EINTR) continue;
            if (waited < 0 && errno == ECHILD) {
                direct_child_reaped_ = true;
                return ReapProbe::no_longer_a_child;
            }
            return ReapProbe::error;
        }
    }

    void terminate_group_and_reap() noexcept {
        if (pid_ <= 0) return;
        const ReapProbe before = probe_reap();
        if (before == ReapProbe::no_longer_a_child) return;
        // POSIX_SPAWN_SETPGROUP made the child PID the process-group ID, so
        // ordinary descendants cannot outlive a timeout or continue touching
        // helper outputs after capture rejection.
        (void)::kill(-pid_, SIGKILL);
        if (direct_child_reaped_) return;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline) {
            const ReapProbe probe = probe_reap();
            if (probe == ReapProbe::reaped
                || probe == ReapProbe::no_longer_a_child) {
                return;
            }
            (void)::poll(nullptr, 0u, 5);
        }
        (void)probe_reap();
    }

    int read_fd_ = -1;
    pid_t pid_ = 0;
    bool direct_child_reaped_ = false;
    bool completed_ = false;
};

[[nodiscard]] inline std::expected<std::string, std::string> run_capture(
    std::span<const std::string> args,
    RunCaptureLimits limits = {},
    std::span<const RunCaptureInheritedFd> inherited_fds = {})
{
    if (args.empty()) {
        return std::unexpected("empty command");
    }
    if (limits.maximum_output_bytes == 0u
        || limits.timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected("invalid command capture limits");
    }
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        return std::unexpected(shader_cache_message("pipe failed: errno ", errno));
    }
    CaptureFileDescriptor read_owner{pipe_fds[0]};
    CaptureFileDescriptor write_owner{pipe_fds[1]};
    const auto set_close_on_exec = [](int fd) noexcept -> int {
        const int flags = ::fcntl(fd, F_GETFD, 0);
        if (flags < 0) return errno;
        if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) return errno;
        return 0;
    };
    if (const int cloexec_error = set_close_on_exec(read_owner.get());
        cloexec_error != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " could not seal capture read fd: errno ",
            cloexec_error));
    }
    if (const int cloexec_error = set_close_on_exec(write_owner.get());
        cloexec_error != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " could not seal capture write fd: errno ",
            cloexec_error));
    }

    CaptureSpawnFileActions actions;
    if (const int action_error = actions.initialize(); action_error != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " could not initialize spawn actions: errno ",
            action_error));
    }
    const std::array action_errors{
        actions.add_dup2(write_owner.get(), STDOUT_FILENO),
        actions.add_dup2(write_owner.get(), STDERR_FILENO),
        actions.add_close(read_owner.get()),
        actions.add_close(write_owner.get()),
    };
    for (const int action_error : action_errors) {
        if (action_error != 0) {
            return std::unexpected(shader_cache_message(
                args[0], " could not configure spawn actions: errno ",
                action_error));
        }
    }
    for (std::size_t index = 0u; index < inherited_fds.size(); ++index) {
        const RunCaptureInheritedFd inherited = inherited_fds[index];
        if (inherited.source_fd < 0 || inherited.target_fd < 3
            || inherited.source_fd == inherited.target_fd) {
            return std::unexpected(
                "invalid inherited descriptor for command capture");
        }
        for (std::size_t prior = 0u; prior < index; ++prior) {
            if (inherited_fds[prior].target_fd == inherited.target_fd) {
                return std::unexpected(
                    "duplicate inherited descriptor target for command capture");
            }
        }
        if (const int action_error = actions.add_dup2(
                inherited.source_fd, inherited.target_fd);
            action_error != 0) {
            return std::unexpected(shader_cache_message(
                args[0], " could not inherit command descriptor: errno ",
                action_error));
        }
    }

    CaptureSpawnAttributes attributes;
    if (const int attribute_error = attributes.initialize_process_group();
        attribute_error != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " could not isolate capture process group: errno ",
            attribute_error));
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = ::posix_spawnp(
        &pid, argv[0], actions.get(), attributes.get(), argv.data(), environ);
    write_owner.close_now();
    if (rc != 0) {
        return std::unexpected(shader_cache_message(args[0], " spawn failed: errno ", rc));
    }
    CapturedChildProcess child{read_owner.release(), pid};
    const int original_flags = ::fcntl(child.read_fd(), F_GETFL, 0);
    if (original_flags < 0
        || ::fcntl(child.read_fd(), F_SETFL, original_flags | O_NONBLOCK) != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " could not make capture pipe nonblocking: errno ", errno));
    }

    std::string output;
    output.reserve(std::min<std::size_t>(limits.maximum_output_bytes, 4096u));
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + limits.timeout;
    bool pipe_eof = false;
    bool child_exited = false;
    int status = 0;
    while (!pipe_eof || !child_exited) {
        if (!pipe_eof) {
            for (;;) {
                const ssize_t n = ::read(child.read_fd(), buffer, sizeof(buffer));
                if (n > 0) {
                    const std::size_t bytes = static_cast<std::size_t>(n);
                    if (bytes > limits.maximum_output_bytes - output.size()) {
                        return std::unexpected(shader_cache_message(
                            args[0], " exceeded capture output cap of ",
                            limits.maximum_output_bytes, " bytes"));
                    }
                    output.append(buffer, bytes);
                    continue;
                }
                if (n == 0) {
                    pipe_eof = true;
                    child.close_read();
                    break;
                }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                return std::unexpected(shader_cache_message(
                    args[0], " read failed: errno ", errno));
            }
        }

        if (!child_exited) {
            for (;;) {
                const pid_t waited = ::waitpid(pid, &status, WNOHANG);
                if (waited == pid) {
                    child_exited = true;
                    child.mark_reaped();
                    break;
                }
                if (waited == 0) break;
                if (waited < 0 && errno == EINTR) continue;
                return std::unexpected(shader_cache_message(
                    args[0], " waitpid failed: errno ", errno));
            }
        }
        if (pipe_eof && child_exited) break;

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::unexpected(shader_cache_message(
                args[0], " timed out after ", limits.timeout.count(), " ms"));
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        const int poll_timeout = static_cast<int>(std::clamp<std::int64_t>(
            remaining.count(), 1, 100));
        pollfd descriptor{
            .fd = pipe_eof ? -1 : child.read_fd(),
            .events = POLLIN | POLLHUP | POLLERR,
            .revents = 0,
        };
        const int poll_result = ::poll(&descriptor, 1u, poll_timeout);
        if (poll_result < 0 && errno != EINTR) {
            return std::unexpected(shader_cache_message(
                args[0], " poll failed: errno ", errno));
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " exited with status ",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1, ": ", output));
    }
    child.mark_completed();
    return std::expected<std::string, std::string>{
        std::in_place, std::move(output)};
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
    hash = bioseq::common::hash_combine_64(
        hash, "bioseq-metal-binary-archive-v6-qualified-helper-device-wire-v2");
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

namespace sealed_shader_cache_detail {

class Sha256 {
private:
    std::array<std::uint32_t, 8> state_{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::byte, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;

    inline static constexpr std::array<std::uint32_t, 64> round_constants{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    void transform(const std::byte* block) noexcept
    {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16u; ++index) {
            const std::size_t offset = index * 4u;
            words[index] =
                static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[offset])) << 24u
                | static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[offset + 1u])) << 16u
                | static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[offset + 2u])) << 8u
                | static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(block[offset + 3u]));
        }
        for (std::size_t index = 16u; index < words.size(); ++index) {
            const std::uint32_t s0 = std::rotr(words[index - 15u], 7)
                ^ std::rotr(words[index - 15u], 18)
                ^ (words[index - 15u] >> 3u);
            const std::uint32_t s1 = std::rotr(words[index - 2u], 17)
                ^ std::rotr(words[index - 2u], 19)
                ^ (words[index - 2u] >> 10u);
            words[index] = words[index - 16u] + s0
                + words[index - 7u] + s1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 = std::rotr(e, 6)
                ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + sum1 + choose
                + round_constants[index] + words[index];
            const std::uint32_t sum0 = std::rotr(a, 2)
                ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

public:
    void update(std::span<const std::byte> bytes) noexcept
    {
        for (const std::byte value : bytes) {
            buffer_[buffered_++] = value;
            ++total_bytes_;
            if (buffered_ == buffer_.size()) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    void update(std::string_view text) noexcept
    {
        update(std::as_bytes(std::span{text.data(), text.size()}));
    }

    void update_little_endian(std::uint64_t value) noexcept
    {
        std::array<std::byte, 8> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(value & 0xffu);
            value >>= 8u;
        }
        update(bytes);
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finalize() noexcept
    {
        const std::uint64_t bit_length = total_bytes_ * 8u;
        buffer_[buffered_++] = std::byte{0x80};
        if (buffered_ > 56u) {
            while (buffered_ < buffer_.size()) {
                buffer_[buffered_++] = std::byte{0};
            }
            transform(buffer_.data());
            buffered_ = 0;
        }
        while (buffered_ < 56u) {
            buffer_[buffered_++] = std::byte{0};
        }
        for (std::size_t index = 0; index < 8u; ++index) {
            buffer_[63u - index] = static_cast<std::byte>(
                (bit_length >> (index * 8u)) & 0xffu);
        }
        transform(buffer_.data());
        std::array<std::uint8_t, 32> digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            for (std::size_t byte = 0; byte < 4u; ++byte) {
                digest[word * 4u + byte] = static_cast<std::uint8_t>(
                    (state_[word] >> (24u - byte * 8u)) & 0xffu);
            }
        }
        return digest;
    }
};

[[nodiscard]] inline std::uint64_t manifest_checksum(
    std::span<const std::byte> bytes) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const std::byte value : bytes) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ull;
    }
    return hash == 0u ? 0x9e3779b97f4a7c15ull : hash;
}

} // namespace sealed_shader_cache_detail

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
    const bool grouped32_stream_kind =
        kind == ShaderKind::phase_c_grouped32_stream_control
        || kind == ShaderKind::phase_c_grouped32_stream_authorize
        || kind == ShaderKind::phase_c_grouped32_stream_position
        || kind == ShaderKind::phase_c_grouped32_stream_seal;
    if (grouped32_stream_kind) {
        constexpr std::array<std::uint32_t, 20> push_offsets{
            0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 36u,
            40u, 44u, 48u, 52u, 56u, 60u, 64u, 68u, 72u, 76u};
        const auto stream_partition = [&binding_struct]() {
            return binding_struct(0u, 64u,
                {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 36u,
                 40u, 48u, 56u, 60u});
        };
        const auto stream_query_record = [&binding_struct]() {
            return binding_struct(2u, 48u,
                {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 40u, 44u});
        };
        const auto stream_query_minimizer = [&binding_struct]() {
            return binding_struct(3u, 16u, {0u, 4u, 8u, 12u});
        };
        const auto stream_occurrence = [&binding_struct]() {
            return binding_struct(4u, 16u, {0u, 4u, 8u, 12u});
        };
        const auto stream_match = [&binding_struct]() {
            return binding_struct(5u, 16u, {0u, 4u, 8u, 12u});
        };
        const auto stream_state = [&binding_struct]() {
            return binding_struct(6u, 64u,
                {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 36u,
                 40u, 44u, 48u, 52u, 56u, 60u});
        };
        const auto stream_anchor = [&binding_struct]() {
            return binding_struct(8u, 24u, {0u, 8u, 16u, 20u});
        };
        const bool push_ok = scan.has_push_constant_offsets(push_offsets);
        bool layout_ok = push_ok && stream_state();
        if (kind == ShaderKind::phase_c_grouped32_stream_control) {
            layout_ok = layout_ok && stream_partition()
                && binding_scalar(1u, 4u) && stream_query_record()
                && stream_query_minimizer() && stream_occurrence()
                && stream_match() && binding_scalar(7u, 4u);
        } else if (kind == ShaderKind::phase_c_grouped32_stream_authorize) {
            layout_ok = layout_ok && stream_occurrence() && stream_match()
                && binding_scalar(7u, 4u);
        } else if (kind == ShaderKind::phase_c_grouped32_stream_position) {
            layout_ok = layout_ok && stream_partition()
                && binding_scalar(1u, 4u) && stream_query_record()
                && stream_query_minimizer() && stream_occurrence()
                && stream_match() && stream_anchor();
        } else {
            layout_ok = layout_ok && stream_occurrence() && stream_anchor();
        }
        if (!layout_ok) return std::unexpected(
            "SPIR-V grouped32 bounded-stream layout mismatch");
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
    if (kind == ShaderKind::sneaky_snake_rank_batch
        && (!binding_scalar(0u, 4u)
            || !binding_scalar(1u, 4u)
            || !binding_struct(2u, 20u, {0u, 4u, 8u, 12u, 16u})
            || !binding_struct(3u, 16u, {0u, 4u, 8u, 12u})
            || !scan.has_push_constant_offsets(
                std::array<std::uint32_t, 2>{0u, 4u}))) {
        return std::unexpected(
            "SPIR-V SneakySnake rank-batch layout mismatch");
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

[[nodiscard]] inline std::expected<ShaderCacheMeta, std::string>
parse_cache_meta_bytes(
    std::span<const std::byte> bytes,
    std::string* entry_name)
{
    if (bytes.size() < sizeof(ShaderCacheMeta)) {
        return std::unexpected("meta too small");
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
    return parse_cache_meta_bytes(bytes, entry_name);
}

[[nodiscard]] inline std::expected<ShaderCacheMeta, std::string>
compatible_cache_meta_from_bytes(
    std::span<const std::byte> bytes,
    const ShaderVariantKey& key,
    std::string* entry_name)
{
    std::string entry;
    auto meta = parse_cache_meta_bytes(bytes, &entry);
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
        *entry_name = std::move(entry);
    }
    return *meta;
}

[[nodiscard]] inline std::expected<ShaderCacheMeta, std::string>
read_compatible_cache_meta(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key,
    std::string* entry_name)
{
    std::ifstream input(cache_meta_path(cache_dir, key), std::ios::binary);
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
    return compatible_cache_meta_from_bytes(bytes, key, entry_name);
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
        if (auto qualified = require_nonempty_cache_file(
                cache_binary_archive_qualification_path(archive),
                "Metal binary archive qualification receipt");
            !qualified) {
            return qualified;
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

namespace sealed_shader_cache_detail {

inline constexpr std::string_view manifest_magic = "BSCSEED3";
inline constexpr std::uint32_t manifest_version = 3u;
inline constexpr std::size_t maximum_manifest_bytes = 16u << 20u;
inline constexpr std::size_t maximum_seed_artifact_bytes = 1u << 30u;
inline constexpr std::uint64_t maximum_seed_image_bytes = 512ull << 20u;
inline constexpr std::size_t maximum_seed_image_artifacts = 16'384u;
inline constexpr std::uint32_t maximum_manifest_keys = 4096u;
inline constexpr std::uint32_t maximum_manifest_artifacts = 65536u;
inline constexpr std::uint32_t maximum_manifest_filename_bytes = 512u;
using SealBeforeIdentityCheckHook = void (*)() noexcept;

struct ExpectedArtifact {
    SealedShaderCacheArtifactKind kind =
        SealedShaderCacheArtifactKind::reserved_command_metallib;
    std::uint32_t key_index = (std::numeric_limits<std::uint32_t>::max)();
    std::string filename;
    std::filesystem::path path;
};

[[nodiscard]] inline bool shader_key_less(
    const ShaderVariantKey& lhs,
    const ShaderVariantKey& rhs) noexcept
{
    return std::tie(lhs.kind, lhs.tile_size, lhs.local_size, lhs.max_span)
        < std::tie(rhs.kind, rhs.tile_size, rhs.local_size, rhs.max_span);
}

[[nodiscard]] inline std::expected<std::vector<ShaderVariantKey>, std::string>
canonical_keys(std::span<const ShaderVariantKey> requested)
{
    if (requested.empty()) {
        return std::unexpected("sealed shader cache key set is empty");
    }
    if (requested.size() > maximum_manifest_keys) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache key count exceeds ", maximum_manifest_keys));
    }
    std::vector<ShaderVariantKey> keys(requested.begin(), requested.end());
    for (const ShaderVariantKey& key : keys) {
        if (!valid_shader_variant_key(key)) {
            return std::unexpected(shader_cache_message(
                "sealed shader cache contains invalid key ",
                cache_file_stem(key)));
        }
    }
    std::sort(keys.begin(), keys.end(), shader_key_less);
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
        return std::unexpected("sealed shader cache key set contains duplicates");
    }
    return keys;
}

[[nodiscard]] inline bool valid_digest(std::string_view digest) noexcept
{
    return digest.size() == 16u
        && std::all_of(digest.begin(), digest.end(), [](char value) {
               return (value >= '0' && value <= '9')
                   || (value >= 'a' && value <= 'f');
           });
}

[[nodiscard]] inline bool safe_filename(std::string_view filename) noexcept
{
    return !filename.empty()
        && filename.size() <= maximum_manifest_filename_bytes
        && filename != "." && filename != ".."
        && filename.find('/') == std::string_view::npos
        && filename.find('\\') == std::string_view::npos
        && filename.find('\0') == std::string_view::npos;
}

[[nodiscard]] inline std::expected<std::vector<ExpectedArtifact>, std::string>
expected_artifacts(
    const std::filesystem::path& cache_dir,
    std::span<const ShaderVariantKey> canonical)
{
    std::vector<ExpectedArtifact> artifacts;
    artifacts.reserve(canonical.size() * 6u);
    const auto append = [&artifacts](
            SealedShaderCacheArtifactKind kind,
            std::uint32_t key_index,
            const std::filesystem::path& path) {
        artifacts.push_back(ExpectedArtifact{
            .kind = kind,
            .key_index = key_index,
            .filename = path.filename().string(),
            .path = path,
        });
    };
    for (std::size_t index = 0; index < canonical.size(); ++index) {
        if (index > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected("sealed shader cache key index overflow");
        }
        const std::uint32_t key_index = static_cast<std::uint32_t>(index);
        append(
            SealedShaderCacheArtifactKind::shader_meta,
            key_index,
            cache_meta_path(cache_dir, canonical[index]));
        append(
            SealedShaderCacheArtifactKind::shader_metallib,
            key_index,
            cache_metallib_path(cache_dir, canonical[index]));
        for (const std::filesystem::path& archive :
             cache_binary_archive_paths(cache_dir, canonical[index])) {
            append(
                SealedShaderCacheArtifactKind::metal_binary_archive,
                key_index,
                archive);
            append(
                SealedShaderCacheArtifactKind::
                    metal_binary_archive_qualification,
                key_index,
                cache_binary_archive_qualification_path(archive));
        }
    }
    if (artifacts.size() > maximum_manifest_artifacts) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache artifact count exceeds ",
            maximum_manifest_artifacts));
    }
    for (const ExpectedArtifact& artifact : artifacts) {
        if (!safe_filename(artifact.filename)) {
            return std::unexpected(shader_cache_message(
                "unsafe generated shader cache filename: ", artifact.filename));
        }
    }
    for (std::size_t index = 0u; index < artifacts.size(); ++index) {
        const auto duplicate = std::find_if(
            artifacts.begin() + static_cast<std::ptrdiff_t>(index + 1u),
            artifacts.end(),
            [&artifacts, index](const ExpectedArtifact& candidate) {
                return candidate.filename == artifacts[index].filename;
            });
        if (duplicate != artifacts.end()) {
            return std::unexpected(shader_cache_message(
                "duplicate generated shader cache filename: ",
                artifacts[index].filename));
        }
    }
    return artifacts;
}

[[nodiscard]] inline std::expected<std::vector<std::byte>, std::string>
read_regular_file(
    const std::filesystem::path& path,
    std::string_view artifact,
    std::size_t maximum_bytes = (std::numeric_limits<std::size_t>::max)(),
    std::optional<std::uint64_t> exact_bytes = std::nullopt)
{
    std::error_code ec;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, ec);
    if (ec) {
        return std::unexpected(shader_cache_message(
            artifact, " stat failed for ", path.string(), ": ", ec.message()));
    }
    if (std::filesystem::is_symlink(status)) {
        return std::unexpected(shader_cache_message(
            artifact, " must not be a symlink: ", path.string()));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(shader_cache_message(
            artifact, " is not a regular file: ", path.string()));
    }
    const std::uintmax_t file_bytes = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(shader_cache_message(
            artifact, " size failed for ", path.string(), ": ", ec.message()));
    }
    if (file_bytes == 0u) {
        return std::unexpected(shader_cache_message(
            artifact, " is empty: ", path.string()));
    }
    if (exact_bytes && file_bytes != *exact_bytes) {
        return std::unexpected(shader_cache_message(
            artifact, " size changed before admitted read: ", path.string(),
            " actual=", file_bytes, " expected=", *exact_bytes));
    }
    if (file_bytes > maximum_bytes
        || file_bytes > (std::numeric_limits<std::size_t>::max)()
        || file_bytes
            > static_cast<std::uintmax_t>(
                (std::numeric_limits<std::streamsize>::max)())) {
        return std::unexpected(shader_cache_message(
            artifact, " is too large: ", path.string(), " bytes=", file_bytes));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(shader_cache_message(
            artifact, " open failed: ", path.string()));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file_bytes));
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        return std::unexpected(shader_cache_message(
            artifact, " short read: ", path.string()));
    }
    return bytes;
}

struct RegularFileIdentity {
    std::uint64_t device = 0u;
    std::uint64_t inode = 0u;
    std::uint64_t size = 0u;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;
    std::int64_t changed_seconds = 0;
    std::int64_t changed_nanoseconds = 0;

    [[nodiscard]] bool operator==(const RegularFileIdentity&) const noexcept =
        default;
};

struct StableRegularFileImage {
    std::vector<std::byte> bytes;
    RegularFileIdentity identity{};
};

class ScopedFileDescriptor {
public:
    explicit ScopedFileDescriptor(int fd) noexcept : fd_(fd) {}
    ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
    ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;
    ~ScopedFileDescriptor() {
        if (fd_ >= 0) (void)::close(fd_);
    }
    [[nodiscard]] int close_now() noexcept {
        const int fd = std::exchange(fd_, -1);
        return fd >= 0 ? ::close(fd) : 0;
    }

private:
    int fd_ = -1;
};

[[nodiscard]] inline RegularFileIdentity regular_file_identity(
    const struct stat& value) noexcept
{
    RegularFileIdentity result{
        .device = static_cast<std::uint64_t>(value.st_dev),
        .inode = static_cast<std::uint64_t>(value.st_ino),
        .size = static_cast<std::uint64_t>(value.st_size),
    };
#if defined(__APPLE__)
    result.modified_seconds = value.st_mtimespec.tv_sec;
    result.modified_nanoseconds = value.st_mtimespec.tv_nsec;
    result.changed_seconds = value.st_ctimespec.tv_sec;
    result.changed_nanoseconds = value.st_ctimespec.tv_nsec;
#else
    result.modified_seconds = value.st_mtim.tv_sec;
    result.modified_nanoseconds = value.st_mtim.tv_nsec;
    result.changed_seconds = value.st_ctim.tv_sec;
    result.changed_nanoseconds = value.st_ctim.tv_nsec;
#endif
    return result;
}

[[nodiscard]] inline std::expected<StableRegularFileImage, std::string>
read_stable_regular_file(
    const std::filesystem::path& path,
    std::string_view artifact,
    std::uint64_t exact_bytes,
    std::uint64_t maximum_bytes)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return std::unexpected(shader_cache_message(
            artifact, " stable open failed for ", path.string(),
            ": errno ", errno));
    }
    ScopedFileDescriptor fd_owner{fd};
    const auto fail = [&path, artifact](std::string_view reason)
        -> std::expected<StableRegularFileImage, std::string> {
        const int saved = errno;
        return std::unexpected(shader_cache_message(
            artifact, ' ', reason, " for ", path.string(),
            ": errno ", saved));
    };
    struct stat before {};
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)
        || before.st_size <= 0
        || static_cast<std::uint64_t>(before.st_size) != exact_bytes
        || exact_bytes > maximum_bytes
        || exact_bytes > (std::numeric_limits<std::size_t>::max)()) {
        return fail("stable pre-read identity invalid");
    }
    StableRegularFileImage image{};
    image.bytes.resize(static_cast<std::size_t>(exact_bytes));
    std::size_t consumed = 0u;
    while (consumed < image.bytes.size()) {
        const ssize_t count = ::read(
            fd, image.bytes.data() + consumed,
            image.bytes.size() - consumed);
        if (count > 0) {
            consumed += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return fail("stable read failed");
    }
    struct stat after {};
    if (::fstat(fd, &after) != 0
        || regular_file_identity(before) != regular_file_identity(after)) {
        return fail("identity changed during stable read");
    }
    if (fd_owner.close_now() != 0) {
        return std::unexpected(shader_cache_message(
            artifact, " stable close failed for ", path.string(),
            ": errno ", errno));
    }
    image.identity = regular_file_identity(after);
    return image;
}

[[nodiscard]] inline bool same_regular_file_identity(
    const std::filesystem::path& path,
    const RegularFileIdentity& expected) noexcept
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat current {};
    const bool accepted = ::fstat(fd, &current) == 0
        && S_ISREG(current.st_mode)
        && regular_file_identity(current) == expected;
    (void)::close(fd);
    return accepted;
}

struct StableRegularFileDigest {
    std::uint64_t size = 0u;
    std::array<std::uint8_t, 32> sha256{};
    RegularFileIdentity identity{};
};

[[nodiscard]] inline std::expected<StableRegularFileDigest, std::string>
stream_stable_regular_file_digest(
    const std::filesystem::path& path,
    std::string_view artifact,
    std::uint64_t maximum_bytes)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return std::unexpected(shader_cache_message(
            artifact, " stream open failed for ", path.string(),
            ": errno ", errno));
    }
    ScopedFileDescriptor fd_owner{fd};
    struct stat before {};
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)
        || before.st_size <= 0
        || static_cast<std::uint64_t>(before.st_size) > maximum_bytes) {
        return std::unexpected(shader_cache_message(
            artifact, " stream pre-read identity invalid for ",
            path.string()));
    }
    constexpr std::size_t scratch_bytes = 64u << 10u;
    std::array<std::byte, scratch_bytes> scratch{};
    Sha256 hasher;
    std::uint64_t streamed = 0u;
    for (;;) {
        const ssize_t count = ::read(fd, scratch.data(), scratch.size());
        if (count > 0) {
            const auto chunk = static_cast<std::size_t>(count);
            if (chunk > maximum_bytes - streamed) {
                return std::unexpected(shader_cache_message(
                    artifact, " grew beyond stream bound for ",
                    path.string()));
            }
            hasher.update(std::span<const std::byte>{scratch.data(), chunk});
            streamed += chunk;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            return std::unexpected(shader_cache_message(
                artifact, " stream read failed for ", path.string(),
                ": errno ", errno));
        }
        break;
    }
    struct stat after {};
    if (::fstat(fd, &after) != 0
        || regular_file_identity(before) != regular_file_identity(after)
        || streamed != static_cast<std::uint64_t>(after.st_size)) {
        return std::unexpected(shader_cache_message(
            artifact, " identity changed during stream for ",
            path.string()));
    }
    if (fd_owner.close_now() != 0) {
        return std::unexpected(shader_cache_message(
            artifact, " stream close failed for ", path.string(),
            ": errno ", errno));
    }
    return StableRegularFileDigest{
        .size = streamed,
        .sha256 = hasher.finalize(),
        .identity = regular_file_identity(after),
    };
}

[[nodiscard]] inline std::array<std::uint8_t, 32> sha256(
    std::span<const std::byte> bytes) noexcept
{
    Sha256 hasher;
    hasher.update(bytes);
    return hasher.finalize();
}

[[nodiscard]] inline std::expected<
    std::pair<std::array<std::uint8_t, 32>, std::uint64_t>,
    std::string>
seed_identity(
    std::string_view cache_digest,
    std::span<const ShaderVariantKey> keys,
    std::span<const SealedShaderCacheArtifact> artifacts)
{
    Sha256 hasher;
    hasher.update("bioseq-sealed-shader-seed-v1");
    hasher.update_little_endian(cache_digest.size());
    hasher.update(cache_digest);
    hasher.update_little_endian(keys.size());
    for (const ShaderVariantKey& key : keys) {
        hasher.update_little_endian(key.kind);
        hasher.update_little_endian(key.tile_size);
        hasher.update_little_endian(key.local_size);
        hasher.update_little_endian(key.max_span);
    }
    hasher.update_little_endian(artifacts.size());
    std::uint64_t seed_size = 0u;
    for (const SealedShaderCacheArtifact& artifact : artifacts) {
        if (artifact.size > (std::numeric_limits<std::uint64_t>::max)()
                - seed_size) {
            return std::unexpected("sealed shader cache seed size overflow");
        }
        seed_size += artifact.size;
        hasher.update_little_endian(static_cast<std::uint32_t>(artifact.kind));
        hasher.update_little_endian(artifact.key_index);
        hasher.update_little_endian(artifact.filename.size());
        hasher.update(artifact.filename);
        hasher.update_little_endian(artifact.size);
        hasher.update(std::as_bytes(std::span{artifact.sha256}));
    }
    return std::pair{hasher.finalize(), seed_size};
}

inline void append_u32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

inline void append_u64(std::vector<std::byte>& bytes, std::uint64_t value)
{
    for (unsigned shift = 0u; shift < 64u; shift += 8u) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

inline void append_text(std::vector<std::byte>& bytes, std::string_view text)
{
    const auto view = std::as_bytes(std::span{text.data(), text.size()});
    bytes.insert(bytes.end(), view.begin(), view.end());
}

inline void append_digest(
    std::vector<std::byte>& bytes,
    const std::array<std::uint8_t, 32>& digest)
{
    const auto view = std::as_bytes(std::span{digest});
    bytes.insert(bytes.end(), view.begin(), view.end());
}

[[nodiscard]] inline std::expected<
    std::pair<std::vector<std::byte>, std::uint64_t>,
    std::string>
serialize_manifest(const SealedShaderCacheManifest& manifest)
{
    if (!valid_digest(manifest.cache_digest)) {
        return std::unexpected("sealed shader cache digest is not lowercase hex16");
    }
    if (manifest.keys.empty()
        || manifest.keys.size() > maximum_manifest_keys
        || manifest.artifacts.empty()
        || manifest.artifacts.size() > maximum_manifest_artifacts) {
        return std::unexpected("sealed shader cache manifest count is invalid");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(256u + manifest.artifacts.size() * 128u);
    append_text(bytes, manifest_magic);
    append_u32(bytes, manifest_version);
    append_u32(bytes, 0u);
    append_u32(bytes, static_cast<std::uint32_t>(manifest.cache_digest.size()));
    append_u32(bytes, static_cast<std::uint32_t>(manifest.keys.size()));
    append_u32(bytes, static_cast<std::uint32_t>(manifest.artifacts.size()));
    append_u32(bytes, 0u);
    append_text(bytes, manifest.cache_digest);
    for (const ShaderVariantKey& key : manifest.keys) {
        append_u32(bytes, key.kind);
        append_u32(bytes, key.tile_size);
        append_u32(bytes, key.local_size);
        append_u32(bytes, key.max_span);
    }
    for (const SealedShaderCacheArtifact& artifact : manifest.artifacts) {
        if (!safe_filename(artifact.filename)) {
            return std::unexpected(shader_cache_message(
                "unsafe shader cache manifest filename: ", artifact.filename));
        }
        append_u32(bytes, static_cast<std::uint32_t>(artifact.kind));
        append_u32(bytes, artifact.key_index);
        append_u32(bytes, static_cast<std::uint32_t>(artifact.filename.size()));
        append_u32(bytes, 0u);
        append_u64(bytes, artifact.size);
        append_digest(bytes, artifact.sha256);
        append_text(bytes, artifact.filename);
    }
    append_u64(bytes, manifest.seed_size);
    append_digest(bytes, manifest.seed_sha256);
    const std::uint64_t checksum = manifest_checksum(bytes);
    append_u64(bytes, checksum);
    if (bytes.size() > maximum_manifest_bytes) {
        return std::unexpected("sealed shader cache manifest is too large");
    }
    return std::pair{std::move(bytes), checksum};
}

class ManifestReader {
private:
    std::span<const std::byte> bytes_;
    std::size_t cursor_ = 0u;

public:
    explicit ManifestReader(std::span<const std::byte> bytes) noexcept
        : bytes_(bytes)
    {}

    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] bool at_end() const noexcept { return cursor_ == bytes_.size(); }

    [[nodiscard]] std::expected<std::span<const std::byte>, std::string> take(
        std::size_t count)
    {
        if (count > bytes_.size() - cursor_) {
            return std::unexpected("sealed shader cache manifest is truncated");
        }
        const auto result = bytes_.subspan(cursor_, count);
        cursor_ += count;
        return result;
    }

    [[nodiscard]] std::expected<std::uint32_t, std::string> u32()
    {
        auto raw = take(4u);
        if (!raw) return std::unexpected(raw.error());
        std::uint32_t value = 0u;
        for (std::size_t index = 0; index < raw->size(); ++index) {
            value |= static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>((*raw)[index]))
                << (index * 8u);
        }
        return value;
    }

    [[nodiscard]] std::expected<std::uint64_t, std::string> u64()
    {
        auto raw = take(8u);
        if (!raw) return std::unexpected(raw.error());
        std::uint64_t value = 0u;
        for (std::size_t index = 0; index < raw->size(); ++index) {
            value |= static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>((*raw)[index]))
                << (index * 8u);
        }
        return value;
    }

    [[nodiscard]] std::expected<std::string, std::string> text(std::size_t count)
    {
        auto raw = take(count);
        if (!raw) return std::unexpected(raw.error());
        return std::string(
            reinterpret_cast<const char*>(raw->data()), raw->size());
    }

    [[nodiscard]] std::expected<std::array<std::uint8_t, 32>, std::string>
    digest()
    {
        auto raw = take(32u);
        if (!raw) return std::unexpected(raw.error());
        std::array<std::uint8_t, 32> result{};
        std::memcpy(result.data(), raw->data(), result.size());
        return result;
    }
};

inline constexpr std::string_view archive_qualification_magic = "BAMISS01";
inline constexpr std::uint32_t archive_qualification_legacy_version = 1u;
inline constexpr std::uint32_t archive_qualification_version = 2u;
inline constexpr std::size_t maximum_archive_qualification_bytes = 4096u;

[[nodiscard]] inline std::expected<std::vector<std::byte>, std::string>
serialize_metal_archive_qualification_receipt(
    const MetalArchiveQualificationReceipt& receipt)
{
    if (!receipt.accepted()) {
        return std::unexpected("Metal archive qualification receipt is incomplete");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(
        256u + receipt.entry_name.size()
        + receipt.negative_error.domain.size()
        + receipt.helper_build_identity.size()
        + receipt.helper_source.size());
    append_text(bytes, archive_qualification_magic);
    append_u32(bytes, archive_qualification_version);
    append_u32(bytes, 0u);
    append_u32(bytes, receipt.shader_key.kind);
    append_u32(bytes, receipt.shader_key.tile_size);
    append_u32(bytes, receipt.shader_key.local_size);
    append_u32(bytes, receipt.shader_key.max_span);
    append_u64(bytes, receipt.metallib_size);
    append_digest(bytes, receipt.metallib_sha256);
    append_u32(bytes, static_cast<std::uint32_t>(receipt.entry_name.size()));
    append_u64(bytes, receipt.max_total_threads_per_threadgroup);
    append_u32(bytes, receipt.threadgroup_size_is_multiple ? 1u : 0u);
    append_u32(
        bytes,
        static_cast<std::uint32_t>(receipt.negative_error.domain.size()));
    append_u64(bytes, std::bit_cast<std::uint64_t>(receipt.negative_error.code));
    append_u64(bytes, receipt.archive_size);
    append_digest(bytes, receipt.archive_sha256);
    append_u32(bytes, receipt.helper_protocol_version);
    append_u32(
        bytes,
        static_cast<std::uint32_t>(receipt.helper_build_identity.size()));
    append_u32(bytes, static_cast<std::uint32_t>(receipt.helper_source.size()));
    append_u32(bytes, 0u);
    append_u64(bytes, receipt.helper_executable_size);
    append_digest(bytes, receipt.helper_executable_sha256);
    append_u64(bytes, receipt.helper_metal_registry_id);
    append_text(bytes, receipt.entry_name);
    append_text(bytes, receipt.negative_error.domain);
    append_text(bytes, receipt.helper_build_identity);
    append_text(bytes, receipt.helper_source);
    append_u64(bytes, manifest_checksum(bytes));
    if (bytes.size() > maximum_archive_qualification_bytes) {
        return std::unexpected("Metal archive qualification receipt is too large");
    }
    return bytes;
}

[[nodiscard]] inline std::expected<MetalArchiveQualificationReceipt, std::string>
parse_metal_archive_qualification_receipt(
    std::span<const std::byte> bytes)
{
    constexpr std::size_t minimum_fixed_bytes = 8u + 2u * 4u + 8u;
    if (bytes.size() < minimum_fixed_bytes
        || bytes.size() > maximum_archive_qualification_bytes) {
        return std::unexpected("Metal archive qualification receipt size is invalid");
    }
    const std::size_t checksum_offset = bytes.size() - sizeof(std::uint64_t);
    ManifestReader reader(bytes);
    auto magic = reader.text(archive_qualification_magic.size());
    auto version = reader.u32();
    auto reserved = reader.u32();
    if (!magic || !version || !reserved
        || *magic != archive_qualification_magic || *reserved != 0u) {
        return std::unexpected(
            "Metal archive qualification receipt header is invalid");
    }
    if (*version == archive_qualification_legacy_version) {
        auto max_threads = reader.u64();
        auto multiple = reader.u32();
        auto domain_size = reader.u32();
        auto code_bits = reader.u64();
        auto archive_size = reader.u64();
        auto archive_sha = reader.digest();
        if (!max_threads || !multiple || !domain_size || !code_bits
            || !archive_size || !archive_sha || *multiple > 1u
            || *domain_size == 0u || *domain_size > 1024u) {
            return std::unexpected(
                "legacy Metal archive qualification receipt is invalid");
        }
        auto domain = reader.text(*domain_size);
        auto stored_checksum = reader.u64();
        if (!domain || !stored_checksum || !reader.at_end()
            || *stored_checksum
                != manifest_checksum(bytes.first(checksum_offset))) {
            return std::unexpected(
                "legacy Metal archive qualification receipt payload is invalid");
        }
        MetalArchiveQualificationReceipt diagnostic{
            .wire_version = archive_qualification_legacy_version,
            .max_total_threads_per_threadgroup = *max_threads,
            .threadgroup_size_is_multiple = *multiple != 0u,
            .negative_error = MetalArchiveNegativeErrorFingerprint{
                .domain = std::move(*domain),
                .code = std::bit_cast<std::int64_t>(*code_bits),
            },
            .archive_size = *archive_size,
            .archive_sha256 = *archive_sha,
        };
        if (!diagnostic.diagnostic_valid() || diagnostic.accepted()) {
            return std::unexpected(
                "legacy Metal archive qualification receipt values are invalid");
        }
        return diagnostic;
    }
    if (*version != archive_qualification_version) {
        return std::unexpected(
            "Metal archive qualification receipt version is unsupported");
    }
    auto key_kind = reader.u32();
    auto key_tile = reader.u32();
    auto key_local = reader.u32();
    auto key_span = reader.u32();
    auto metallib_size = reader.u64();
    auto metallib_sha = reader.digest();
    auto entry_size = reader.u32();
    auto max_threads = reader.u64();
    auto multiple = reader.u32();
    auto domain_size = reader.u32();
    auto code_bits = reader.u64();
    auto archive_size = reader.u64();
    auto archive_sha = reader.digest();
    auto helper_protocol = reader.u32();
    auto helper_build_size = reader.u32();
    auto helper_source_size = reader.u32();
    auto reserved2 = reader.u32();
    auto helper_executable_size = reader.u64();
    auto helper_executable_sha = reader.digest();
    auto helper_registry_id = reader.u64();
    if (!key_kind || !key_tile || !key_local || !key_span
        || !metallib_size || !metallib_sha || !entry_size
        || !max_threads || !multiple || !domain_size || !code_bits
        || !archive_size || !archive_sha || !helper_protocol
        || !helper_build_size || !helper_source_size || !reserved2
        || !helper_executable_size || !helper_executable_sha
        || !helper_registry_id) {
        return std::unexpected("Metal archive qualification receipt is truncated");
    }
    if (*reserved2 != 0u || *multiple > 1u
        || *entry_size == 0u || *entry_size > 1024u
        || *domain_size == 0u || *domain_size > 1024u
        || *helper_build_size == 0u || *helper_build_size > 1024u
        || *helper_source_size == 0u || *helper_source_size > 64u) {
        return std::unexpected(
            "Metal archive qualification receipt header is invalid");
    }
    auto entry_name = reader.text(*entry_size);
    auto domain = reader.text(*domain_size);
    auto helper_build = reader.text(*helper_build_size);
    auto helper_source = reader.text(*helper_source_size);
    auto stored_checksum = reader.u64();
    if (!entry_name || !domain || !helper_build || !helper_source
        || !stored_checksum || !reader.at_end()) {
        return std::unexpected(
            "Metal archive qualification receipt payload is invalid");
    }
    if (*stored_checksum != manifest_checksum(bytes.first(checksum_offset))) {
        return std::unexpected(
            "Metal archive qualification receipt checksum mismatch");
    }
    MetalArchiveQualificationReceipt receipt{
        .wire_version = archive_qualification_version,
        .shader_key = ShaderVariantKey{
            .kind = *key_kind,
            .tile_size = *key_tile,
            .local_size = *key_local,
            .max_span = *key_span,
        },
        .metallib_size = *metallib_size,
        .metallib_sha256 = *metallib_sha,
        .entry_name = std::move(*entry_name),
        .max_total_threads_per_threadgroup = *max_threads,
        .threadgroup_size_is_multiple = *multiple != 0u,
        .negative_error = MetalArchiveNegativeErrorFingerprint{
            .domain = std::move(*domain),
            .code = std::bit_cast<std::int64_t>(*code_bits),
        },
        .archive_size = *archive_size,
        .archive_sha256 = *archive_sha,
        .helper_protocol_version = *helper_protocol,
        .helper_build_identity = std::move(*helper_build),
        .helper_source = std::move(*helper_source),
        .helper_executable_size = *helper_executable_size,
        .helper_executable_sha256 = *helper_executable_sha,
        .helper_metal_registry_id = *helper_registry_id,
    };
    if (!receipt.accepted()) {
        return std::unexpected(
            "Metal archive qualification receipt values are invalid");
    }
    return receipt;
}

[[nodiscard]] inline std::expected<SealedShaderCacheManifest, std::string>
parse_manifest(std::span<const std::byte> bytes)
{
    if (bytes.size() > maximum_manifest_bytes) {
        return std::unexpected("sealed shader cache manifest is too large");
    }
    if (bytes.size() < manifest_magic.size() + 6u * sizeof(std::uint32_t)
            + 16u + sizeof(std::uint64_t) + 32u + sizeof(std::uint64_t)) {
        return std::unexpected("sealed shader cache manifest is too small");
    }
    ManifestReader reader(bytes);
    auto magic = reader.text(manifest_magic.size());
    if (!magic || *magic != manifest_magic) {
        return std::unexpected("sealed shader cache manifest magic mismatch");
    }
    auto version = reader.u32();
    auto reserved0 = reader.u32();
    auto digest_size = reader.u32();
    auto key_count = reader.u32();
    auto artifact_count = reader.u32();
    auto reserved1 = reader.u32();
    if (!version || !reserved0 || !digest_size || !key_count
        || !artifact_count || !reserved1) {
        return std::unexpected("sealed shader cache manifest header is truncated");
    }
    if (*version != manifest_version || *reserved0 != 0u || *reserved1 != 0u) {
        return std::unexpected("sealed shader cache manifest version/reserved mismatch");
    }
    if (*digest_size != 16u
        || *key_count == 0u || *key_count > maximum_manifest_keys
        || *artifact_count == 0u
        || *artifact_count > maximum_manifest_artifacts) {
        return std::unexpected("sealed shader cache manifest header count is invalid");
    }

    SealedShaderCacheManifest manifest{};
    auto cache_digest = reader.text(*digest_size);
    if (!cache_digest || !valid_digest(*cache_digest)) {
        return std::unexpected("sealed shader cache manifest digest is invalid");
    }
    manifest.cache_digest = std::move(*cache_digest);
    manifest.keys.reserve(*key_count);
    for (std::uint32_t index = 0u; index < *key_count; ++index) {
        auto kind = reader.u32();
        auto tile_size = reader.u32();
        auto local_size = reader.u32();
        auto max_span = reader.u32();
        if (!kind || !tile_size || !local_size || !max_span) {
            return std::unexpected("sealed shader cache key table is truncated");
        }
        manifest.keys.push_back(ShaderVariantKey{
            .kind = *kind,
            .tile_size = *tile_size,
            .local_size = *local_size,
            .max_span = *max_span,
        });
    }
    manifest.artifacts.reserve(*artifact_count);
    for (std::uint32_t index = 0u; index < *artifact_count; ++index) {
        auto raw_kind = reader.u32();
        auto key_index = reader.u32();
        auto filename_size = reader.u32();
        auto reserved = reader.u32();
        auto size = reader.u64();
        auto digest = reader.digest();
        if (!raw_kind || !key_index || !filename_size || !reserved
            || !size || !digest) {
            return std::unexpected("sealed shader cache artifact table is truncated");
        }
        if (*raw_kind
                > static_cast<std::uint32_t>(
                    SealedShaderCacheArtifactKind::
                        metal_binary_archive_qualification)
            || *filename_size == 0u
            || *filename_size > maximum_manifest_filename_bytes
            || *reserved != 0u || *size == 0u) {
            return std::unexpected("sealed shader cache artifact record is invalid");
        }
        auto filename = reader.text(*filename_size);
        if (!filename || !safe_filename(*filename)) {
            return std::unexpected("sealed shader cache artifact filename is unsafe");
        }
        manifest.artifacts.push_back(SealedShaderCacheArtifact{
            .kind = static_cast<SealedShaderCacheArtifactKind>(*raw_kind),
            .key_index = *key_index,
            .filename = std::move(*filename),
            .size = *size,
            .sha256 = *digest,
        });
    }
    auto seed_size = reader.u64();
    auto seed_sha256 = reader.digest();
    if (!seed_size || !seed_sha256) {
        return std::unexpected("sealed shader cache seed identity is truncated");
    }
    manifest.seed_size = *seed_size;
    manifest.seed_sha256 = *seed_sha256;
    const std::size_t checksum_offset = reader.cursor();
    auto checksum = reader.u64();
    if (!checksum || !reader.at_end()) {
        return std::unexpected("sealed shader cache manifest has trailing/truncated data");
    }
    manifest.manifest_checksum = *checksum;
    const std::uint64_t computed = manifest_checksum(bytes.first(checksum_offset));
    if (*checksum == 0u || *checksum != computed) {
        return std::unexpected("sealed shader cache manifest checksum mismatch");
    }
    return manifest;
}

[[nodiscard]] inline std::expected<void, std::string> validate_shape(
    const std::filesystem::path& cache_dir,
    const SealedShaderCacheManifest& manifest,
    std::string_view expected_digest,
    std::span<const ShaderVariantKey> expected_keys)
{
    if (!valid_digest(expected_digest) || manifest.cache_digest != expected_digest) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache digest mismatch: manifest=",
            manifest.cache_digest, " expected=", expected_digest));
    }
    auto canonical = canonical_keys(expected_keys);
    if (!canonical) return std::unexpected(canonical.error());
    if (manifest.keys != *canonical) {
        return std::unexpected("sealed shader cache key membership mismatch");
    }
    auto expected = expected_artifacts(cache_dir, *canonical);
    if (!expected) return std::unexpected(expected.error());
    if (manifest.artifacts.size() != expected->size()) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache artifact membership count mismatch: manifest=",
            manifest.artifacts.size(), " expected=", expected->size()));
    }
    for (std::size_t index = 0u; index < expected->size(); ++index) {
        const SealedShaderCacheArtifact& actual = manifest.artifacts[index];
        const ExpectedArtifact& wanted = (*expected)[index];
        if (actual.kind != wanted.kind
            || actual.key_index != wanted.key_index
            || actual.filename != wanted.filename) {
            return std::unexpected(shader_cache_message(
                "sealed shader cache artifact membership mismatch at ordinal ",
                index));
        }
    }
    auto seed = seed_identity(
        manifest.cache_digest, manifest.keys, manifest.artifacts);
    if (!seed) return std::unexpected(seed.error());
    if (manifest.seed_sha256 != seed->first
        || manifest.seed_size != seed->second) {
        return std::unexpected("sealed shader cache seed identity mismatch");
    }
    return {};
}

[[nodiscard]] inline std::expected<void, std::string>
validate_artifact_bytes(
    const SealedShaderCacheArtifact& artifact,
    std::span<const std::byte> bytes)
{
    if (bytes.size() != artifact.size) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache artifact size mismatch for ",
            artifact.filename, ": actual=", bytes.size(),
            " manifest=", artifact.size));
    }
    if (sha256(bytes) != artifact.sha256) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache artifact SHA-256 mismatch for ",
            artifact.filename));
    }
    return {};
}

[[nodiscard]] inline std::expected<std::vector<std::byte>, std::string>
load_validated_artifact_bytes(
    const std::filesystem::path& cache_dir,
    const SealedShaderCacheArtifact& artifact)
{
    auto bytes = read_regular_file(
        cache_dir / artifact.filename,
        "sealed shader cache artifact",
        maximum_seed_artifact_bytes);
    if (!bytes) return std::unexpected(bytes.error());
    if (auto valid = validate_artifact_bytes(artifact, *bytes); !valid) {
        return std::unexpected(valid.error());
    }
    return bytes;
}

[[nodiscard]] inline std::expected<void, std::string> validate_artifact(
    const std::filesystem::path& cache_dir,
    const SealedShaderCacheArtifact& artifact)
{
    auto bytes = load_validated_artifact_bytes(cache_dir, artifact);
    if (!bytes) return std::unexpected(bytes.error());
    return {};
}

[[nodiscard]] inline std::expected<void, std::string> validate_all_artifacts(
    const std::filesystem::path& cache_dir,
    const SealedShaderCacheManifest& manifest)
{
    for (const SealedShaderCacheArtifact& artifact : manifest.artifacts) {
        if (auto valid = validate_artifact(cache_dir, artifact); !valid) {
            return valid;
        }
    }
    for (const ShaderVariantKey& key : manifest.keys) {
        if (auto status = cache_entry_status(cache_dir, key); !status) {
            return std::unexpected(shader_cache_message(
                "sealed shader cache entry is incompatible for ",
                cache_file_stem(key), ": ", status.error()));
        }
    }
    std::optional<MetalArchiveNegativeErrorFingerprint> common_negative_error;
    using HelperIdentity = std::tuple<
        std::uint32_t, std::string, std::string, std::uint64_t,
        std::array<std::uint8_t, 32>, std::uint64_t>;
    std::optional<HelperIdentity> common_helper;
    for (const SealedShaderCacheArtifact& archive : manifest.artifacts) {
        if (archive.kind
            != SealedShaderCacheArtifactKind::metal_binary_archive) {
            continue;
        }
        const std::string qualification_filename =
            archive.filename + ".qualification";
        const auto qualification_artifact = std::find_if(
            manifest.artifacts.begin(),
            manifest.artifacts.end(),
            [&archive, &qualification_filename](
                const SealedShaderCacheArtifact& candidate) {
                return candidate.kind
                        == SealedShaderCacheArtifactKind::
                            metal_binary_archive_qualification
                    && candidate.key_index == archive.key_index
                    && candidate.filename == qualification_filename;
            });
        if (qualification_artifact == manifest.artifacts.end()) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification missing for ", archive.filename));
        }
        auto qualification_bytes = load_validated_artifact_bytes(
            cache_dir, *qualification_artifact);
        if (!qualification_bytes) {
            return std::unexpected(qualification_bytes.error());
        }
        auto qualification = parse_metal_archive_qualification_receipt(
            *qualification_bytes);
        if (!qualification) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification invalid for ", archive.filename,
                ": ", qualification.error()));
        }
        if (!qualification->production_accepted()) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification is not production v2 for ",
                archive.filename));
        }
#if defined(__APPLE__)
        if (auto current =
                validate_metal_archive_receipt_against_process_helper(
                    *qualification);
            !current) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification helper mismatch for ",
                archive.filename, ": ", current.error()));
        }
#endif
        if (qualification->archive_size != archive.size
            || qualification->archive_sha256 != archive.sha256) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification is bound to different bytes for ",
                archive.filename));
        }
        if (archive.key_index >= manifest.keys.size()) {
            return std::unexpected(
                "Metal archive qualification key index is out of range");
        }
        const ShaderVariantKey& key = manifest.keys[archive.key_index];
        const std::string stem = cache_file_stem(key);
        const auto metallib_artifact = std::find_if(
            manifest.artifacts.begin(), manifest.artifacts.end(),
            [archive_key = archive.key_index, &stem](
                const SealedShaderCacheArtifact& candidate) {
                return candidate.kind
                        == SealedShaderCacheArtifactKind::shader_metallib
                    && candidate.key_index == archive_key
                    && candidate.filename == stem + ".metallib";
            });
        std::string expected_entry_name;
        auto meta = read_compatible_cache_meta(
            cache_dir, key, &expected_entry_name);
        if (metallib_artifact == manifest.artifacts.end() || !meta
            || qualification->shader_key != key
            || qualification->metallib_size != metallib_artifact->size
            || qualification->metallib_sha256 != metallib_artifact->sha256
            || qualification->entry_name != expected_entry_name) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification key/metallib/entry mismatch for ",
                archive.filename));
        }
        const auto paths = cache_binary_archive_paths(cache_dir, key);
        const auto path = std::find_if(
            paths.begin(), paths.end(), [&archive](
                const std::filesystem::path& candidate) {
                return candidate.filename() == archive.filename;
            });
        if (path == paths.end()) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification has unknown descriptor cap for ",
                archive.filename));
        }
        const ShaderWorkgroupSize workgroup =
            expected_shader_workgroup_size(key);
        const std::uint64_t exact_threads =
            static_cast<std::uint64_t>(workgroup.x)
            * static_cast<std::uint64_t>(workgroup.y)
            * static_cast<std::uint64_t>(workgroup.z);
        const std::uint64_t expected_threads = path == paths.begin()
            ? exact_threads
            : 256u;
        if (qualification->max_total_threads_per_threadgroup
                != expected_threads
            || qualification->threadgroup_size_is_multiple) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification descriptor mismatch for ",
                archive.filename));
        }
        if (common_negative_error
            && *common_negative_error != qualification->negative_error) {
            return std::unexpected(
                "Metal archive qualifications disagree on NSError fingerprint");
        }
        common_negative_error = qualification->negative_error;
        HelperIdentity helper_identity{
            qualification->helper_protocol_version,
            qualification->helper_build_identity,
            qualification->helper_source,
            qualification->helper_executable_size,
            qualification->helper_executable_sha256,
            qualification->helper_metal_registry_id,
        };
        if (common_helper && *common_helper != helper_identity) {
            return std::unexpected(
                "Metal archive qualifications disagree on helper or registry identity");
        }
        common_helper = std::move(helper_identity);
    }
    if (!common_negative_error || !common_helper) {
        return std::unexpected(
            "sealed shader cache has no Metal archive qualification evidence");
    }
    return {};
}

[[nodiscard]] inline const SealedShaderCacheArtifactImage*
find_artifact_image(
    std::span<const SealedShaderCacheArtifactImage> images,
    SealedShaderCacheArtifactKind kind,
    std::uint32_t key_index,
    std::string_view filename) noexcept
{
    const auto image = std::find_if(
        images.begin(), images.end(),
        [kind, key_index, filename](
            const SealedShaderCacheArtifactImage& candidate) {
            return candidate.descriptor.kind == kind
                && candidate.descriptor.key_index == key_index
                && candidate.descriptor.filename == filename;
        });
    return image == images.end() ? nullptr : &*image;
}

/// Validate all semantic relationships using only the retained artifact
/// bytes. File size/SHA validation happened during their one loader read.
[[nodiscard]] inline std::expected<void, std::string>
validate_all_artifact_images(
    const std::filesystem::path& cache_dir,
    const SealedShaderCacheManifest& manifest,
    std::span<const SealedShaderCacheArtifactImage> images)
{
    if (images.size() != manifest.artifacts.size()) {
        return std::unexpected(
            "sealed shader cache seed image artifact count mismatch");
    }
    for (std::size_t ordinal = 0u; ordinal < images.size(); ++ordinal) {
        if (images[ordinal].descriptor != manifest.artifacts[ordinal]
            || images[ordinal].bytes.empty()) {
            return std::unexpected(shader_cache_message(
                "sealed shader cache seed image mismatch at ordinal ",
                ordinal));
        }
    }

    for (std::size_t key_ordinal = 0u;
         key_ordinal < manifest.keys.size(); ++key_ordinal) {
        const ShaderVariantKey& key = manifest.keys[key_ordinal];
        const std::uint32_t key_index = static_cast<std::uint32_t>(key_ordinal);
        const std::string stem = cache_file_stem(key);
        const auto* metadata = find_artifact_image(
            images, SealedShaderCacheArtifactKind::shader_meta,
            key_index, stem + ".meta");
        const auto* metallib = find_artifact_image(
            images, SealedShaderCacheArtifactKind::shader_metallib,
            key_index, stem + ".metallib");
        if (metadata == nullptr || metallib == nullptr
            || metallib->bytes.empty()) {
            return std::unexpected(shader_cache_message(
                "sealed shader cache seed key image incomplete for ", stem));
        }
        std::string entry_name;
        if (auto meta = compatible_cache_meta_from_bytes(
                metadata->bytes, key, &entry_name);
            !meta || entry_name.empty()) {
            return std::unexpected(shader_cache_message(
                "sealed shader cache seed metadata incompatible for ", stem,
                ": ", meta ? "entry name is empty" : meta.error()));
        }
    }

    std::optional<MetalArchiveNegativeErrorFingerprint> common_negative_error;
    using SeedHelperIdentity = std::tuple<
        std::uint32_t, std::string, std::string, std::uint64_t,
        std::array<std::uint8_t, 32>, std::uint64_t>;
    std::optional<SeedHelperIdentity> common_helper;
    for (const SealedShaderCacheArtifactImage& archive : images) {
        if (archive.descriptor.kind
            != SealedShaderCacheArtifactKind::metal_binary_archive) {
            if (archive.descriptor.kind
                == SealedShaderCacheArtifactKind::reserved_command_metallib) {
                return std::unexpected(
                    "sealed shader cache seed contains forbidden placeholder command artifact");
            }
            continue;
        }
        const std::string qualification_filename =
            archive.descriptor.filename + ".qualification";
        const auto* qualification_image = find_artifact_image(
            images,
            SealedShaderCacheArtifactKind::metal_binary_archive_qualification,
            archive.descriptor.key_index, qualification_filename);
        if (qualification_image == nullptr) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification missing for ",
                archive.descriptor.filename));
        }
        auto qualification = parse_metal_archive_qualification_receipt(
            qualification_image->bytes);
        if (!qualification) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification invalid for ",
                archive.descriptor.filename, ": ", qualification.error()));
        }
        if (!qualification->production_accepted()) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification is not production v2 for ",
                archive.descriptor.filename));
        }
#if defined(__APPLE__)
        if (auto current =
                validate_metal_archive_receipt_against_process_helper(
                    *qualification);
            !current) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification helper mismatch for ",
                archive.descriptor.filename, ": ", current.error()));
        }
#endif
        if (qualification->archive_size != archive.descriptor.size
            || qualification->archive_sha256 != archive.descriptor.sha256) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification is bound to different bytes for ",
                archive.descriptor.filename));
        }
        if (archive.descriptor.key_index >= manifest.keys.size()) {
            return std::unexpected(
                "Metal archive qualification key index is out of range");
        }
        const ShaderVariantKey& key =
            manifest.keys[archive.descriptor.key_index];
        const std::string stem = cache_file_stem(key);
        const auto* metallib = find_artifact_image(
            images, SealedShaderCacheArtifactKind::shader_metallib,
            archive.descriptor.key_index, stem + ".metallib");
        const auto* metadata = find_artifact_image(
            images, SealedShaderCacheArtifactKind::shader_meta,
            archive.descriptor.key_index, stem + ".meta");
        std::string expected_entry_name;
        auto meta = metadata == nullptr
            ? std::expected<ShaderCacheMeta, std::string>{
                std::unexpected("metadata missing")}
            : compatible_cache_meta_from_bytes(
                metadata->bytes, key, &expected_entry_name);
        if (metallib == nullptr || !meta
            || qualification->shader_key != key
            || qualification->metallib_size
                != metallib->descriptor.size
            || qualification->metallib_sha256
                != metallib->descriptor.sha256
            || qualification->entry_name != expected_entry_name) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification key/metallib/entry mismatch for ",
                archive.descriptor.filename));
        }
        const auto paths = cache_binary_archive_paths(cache_dir, key);
        const auto path = std::find_if(
            paths.begin(), paths.end(), [&archive](
                const std::filesystem::path& candidate) {
                return candidate.filename() == archive.descriptor.filename;
            });
        if (path == paths.end()) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification has unknown descriptor cap for ",
                archive.descriptor.filename));
        }
        const ShaderWorkgroupSize workgroup = expected_shader_workgroup_size(key);
        const std::uint64_t exact_threads =
            static_cast<std::uint64_t>(workgroup.x)
            * static_cast<std::uint64_t>(workgroup.y)
            * static_cast<std::uint64_t>(workgroup.z);
        const std::uint64_t expected_threads =
            path == paths.begin() ? exact_threads : 256u;
        if (qualification->max_total_threads_per_threadgroup
                != expected_threads
            || qualification->threadgroup_size_is_multiple) {
            return std::unexpected(shader_cache_message(
                "Metal archive qualification descriptor mismatch for ",
                archive.descriptor.filename));
        }
        if (common_negative_error
            && *common_negative_error != qualification->negative_error) {
            return std::unexpected(
                "Metal archive qualifications disagree on NSError fingerprint");
        }
        common_negative_error = qualification->negative_error;
        SeedHelperIdentity helper_identity{
            qualification->helper_protocol_version,
            qualification->helper_build_identity,
            qualification->helper_source,
            qualification->helper_executable_size,
            qualification->helper_executable_sha256,
            qualification->helper_metal_registry_id,
        };
        if (common_helper && *common_helper != helper_identity) {
            return std::unexpected(
                "Metal archive qualifications disagree on helper or registry identity");
        }
        common_helper = std::move(helper_identity);
    }
    if (!common_negative_error || !common_helper) {
        return std::unexpected(
            "sealed shader cache seed has no Metal archive qualification evidence");
    }
    return {};
}

[[nodiscard]] inline std::uint64_t sealed_seed_receipt_identity(
    const SealedShaderCacheManifest& manifest,
    std::uint64_t artifact_read_count,
    std::uint64_t manifest_bytes,
    std::uint64_t artifact_bytes,
    std::uint64_t load_ns) noexcept
{
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    std::uint64_t identity = fnv_offset;
    const auto append_u64_to_identity = [&identity](std::uint64_t value) {
        constexpr std::uint64_t local_fnv_prime = 1099511628211ull;
        for (std::uint32_t shift = 0u; shift != 64u; shift += 8u) {
            identity ^= static_cast<std::uint8_t>(value >> shift);
            identity *= local_fnv_prime;
        }
    };
    for (const std::uint8_t byte : manifest.seed_sha256) {
        identity ^= byte;
        identity *= fnv_prime;
    }
    append_u64_to_identity(manifest.manifest_checksum);
    append_u64_to_identity(artifact_read_count);
    append_u64_to_identity(manifest_bytes);
    append_u64_to_identity(artifact_bytes);
    append_u64_to_identity(load_ns);
    return identity == 0u ? fnv_offset : identity;
}

[[nodiscard]] inline std::uint64_t seed_image_preallocation_peak_bytes(
    const SealedShaderCacheManifest& manifest,
    std::uint64_t manifest_bytes) noexcept
{
    using bioseq::parallelism::saturating_add;
    using bioseq::parallelism::saturating_mul;
    std::uint64_t retained = sizeof(SealedShaderCacheSeedImage);
    retained = saturating_add(retained, manifest_bytes);
    retained = saturating_add(
        retained, static_cast<std::uint64_t>(manifest.cache_digest.size()));
    retained = saturating_add(
        retained,
        saturating_mul(
            static_cast<std::uint64_t>(manifest.keys.size()),
            sizeof(ShaderVariantKey)));
    retained = saturating_add(
        retained,
        saturating_mul(
            static_cast<std::uint64_t>(manifest.artifacts.size()),
            sizeof(SealedShaderCacheArtifact)
                + sizeof(SealedShaderCacheArtifactImage)));
    std::uint64_t maximum_transient_artifact = 0u;
    for (const SealedShaderCacheArtifact& artifact : manifest.artifacts) {
        retained = saturating_add(
            retained,
            saturating_mul(
                static_cast<std::uint64_t>(artifact.filename.size()), 2u));
        maximum_transient_artifact =
            std::max(maximum_transient_artifact, artifact.size);
    }
    retained = saturating_add(retained, manifest.seed_size);
    const std::uint64_t retained_headroom = saturating_mul(
        retained,
        bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    return saturating_add(retained_headroom, maximum_transient_artifact);
}

struct ManifestAdmissionProbe {
    std::uint64_t file_bytes = 0u;
    std::uint64_t seed_bytes = 0u;
    std::uint32_t key_count = 0u;
    std::uint32_t artifact_count = 0u;
    std::uint64_t peak_admitted_bytes = 0u;
};

[[nodiscard]] inline std::uint32_t probe_u32(
    std::span<const std::byte, 4> bytes) noexcept
{
    std::uint32_t value = 0u;
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(bytes[index])) << (index * 8u);
    }
    return value;
}

[[nodiscard]] inline std::uint64_t probe_u64(
    std::span<const std::byte, 8> bytes) noexcept
{
    std::uint64_t value = 0u;
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(bytes[index])) << (index * 8u);
    }
    return value;
}

[[nodiscard]] inline std::uint64_t seed_image_probe_peak_bytes(
    std::uint64_t manifest_bytes,
    std::uint64_t seed_bytes,
    std::uint32_t key_count,
    std::uint32_t artifact_count) noexcept
{
    using bioseq::parallelism::saturating_add;
    using bioseq::parallelism::saturating_mul;
    std::uint64_t retained = sizeof(SealedShaderCacheSeedImage);
    retained = saturating_add(retained, manifest_bytes);
    retained = saturating_add(retained, 16u);
    retained = saturating_add(
        retained,
        saturating_mul(key_count, sizeof(ShaderVariantKey)));
    retained = saturating_add(
        retained,
        saturating_mul(
            artifact_count,
            sizeof(SealedShaderCacheArtifact)
                + sizeof(SealedShaderCacheArtifactImage)
                + 2u * maximum_manifest_filename_bytes));
    retained = saturating_add(retained, seed_bytes);
    retained = saturating_mul(
        retained, bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    // During each exact read the retained image and one incoming artifact may
    // coexist. The largest possible artifact is the entire bounded seed.
    return saturating_add(retained, seed_bytes);
}

[[nodiscard]] inline std::expected<ManifestAdmissionProbe, std::string>
probe_manifest_for_admission(const std::filesystem::path& path)
{
    constexpr std::size_t header_bytes = 32u;
    constexpr std::size_t tail_bytes = 48u;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache manifest probe open failed: errno ", errno));
    }
    struct stat stat_before {};
    if (::fstat(fd, &stat_before) != 0 || !S_ISREG(stat_before.st_mode)
        || stat_before.st_size <= 0
        || static_cast<std::uint64_t>(stat_before.st_size)
            > maximum_manifest_bytes
        || static_cast<std::uint64_t>(stat_before.st_size)
            < header_bytes + tail_bytes) {
        const int saved = errno;
        ::close(fd);
        return std::unexpected(shader_cache_message(
            "sealed shader cache manifest probe stat/size invalid: errno ",
            saved));
    }
    std::array<std::byte, header_bytes> header{};
    std::array<std::byte, tail_bytes> tail{};
    const auto read_exact = [fd](std::span<std::byte> destination,
                                 off_t offset) noexcept {
        std::size_t consumed = 0u;
        while (consumed < destination.size()) {
            const ssize_t count = ::pread(
                fd, destination.data() + consumed,
                destination.size() - consumed,
                offset + static_cast<off_t>(consumed));
            if (count > 0) {
                consumed += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    };
    const bool read_ok = read_exact(header, 0)
        && read_exact(
            tail, stat_before.st_size - static_cast<off_t>(tail.size()));
    struct stat stat_after {};
    const bool stable = ::fstat(fd, &stat_after) == 0
        && stat_after.st_dev == stat_before.st_dev
        && stat_after.st_ino == stat_before.st_ino
        && stat_after.st_size == stat_before.st_size
        && stat_after.st_mtime == stat_before.st_mtime;
    ::close(fd);
    if (!read_ok || !stable
        || std::memcmp(header.data(), manifest_magic.data(),
                       manifest_magic.size()) != 0
        || probe_u32(std::span<const std::byte, 4>{header.data() + 8u, 4u})
            != manifest_version
        || probe_u32(std::span<const std::byte, 4>{header.data() + 12u, 4u})
            != 0u
        || probe_u32(std::span<const std::byte, 4>{header.data() + 16u, 4u})
            != 16u
        || probe_u32(std::span<const std::byte, 4>{header.data() + 28u, 4u})
            != 0u) {
        return std::unexpected(
            "sealed shader cache manifest admission probe is invalid");
    }
    ManifestAdmissionProbe result{};
    result.file_bytes = static_cast<std::uint64_t>(stat_before.st_size);
    result.key_count = probe_u32(
        std::span<const std::byte, 4>{header.data() + 20u, 4u});
    result.artifact_count = probe_u32(
        std::span<const std::byte, 4>{header.data() + 24u, 4u});
    result.seed_bytes = probe_u64(
        std::span<const std::byte, 8>{tail.data(), 8u});
    if (result.key_count == 0u || result.key_count > maximum_manifest_keys
        || result.artifact_count == 0u
        || result.artifact_count > maximum_seed_image_artifacts
        || result.seed_bytes == 0u
        || result.seed_bytes > maximum_seed_image_bytes) {
        return std::unexpected(
            "sealed shader cache manifest admission probe exceeds bounded ownership");
    }
    result.peak_admitted_bytes = seed_image_probe_peak_bytes(
        result.file_bytes, result.seed_bytes, result.key_count,
        result.artifact_count);
    if (result.peak_admitted_bytes
        == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(
            "sealed shader cache manifest admission probe saturated");
    }
    return result;
}

[[nodiscard]] inline std::expected<SealedShaderCacheManifest, std::string>
read_manifest_shape(
    const std::filesystem::path& cache_dir,
    std::string_view expected_digest,
    std::span<const ShaderVariantKey> expected_keys)
{
    auto bytes = read_regular_file(
        sealed_shader_cache_manifest_path(cache_dir),
        "sealed shader cache manifest",
        maximum_manifest_bytes);
    if (!bytes) return std::unexpected(bytes.error());
    auto manifest = parse_manifest(*bytes);
    if (!manifest) return std::unexpected(manifest.error());
    if (auto valid = validate_shape(
            cache_dir, *manifest, expected_digest, expected_keys);
        !valid) {
        return std::unexpected(valid.error());
    }
    return manifest;
}

} // namespace sealed_shader_cache_detail

[[nodiscard]] inline std::expected<std::vector<ShaderVariantKey>, std::string>
canonical_sealed_shader_cache_keys(
    std::span<const ShaderVariantKey> requested_keys)
{
    return sealed_shader_cache_detail::canonical_keys(requested_keys);
}

[[nodiscard]] inline std::expected<SealedShaderCacheSeedImage, std::string>
load_validated_sealed_shader_cache_seed(
    const std::filesystem::path& cache_dir,
    std::string_view cache_digest,
    std::span<const ShaderVariantKey> expected_keys)
{
    using namespace sealed_shader_cache_detail;
    const auto started = std::chrono::steady_clock::now();
    const std::filesystem::path manifest_path =
        sealed_shader_cache_manifest_path(cache_dir);
    auto admission_probe = probe_manifest_for_admission(manifest_path);
    if (!admission_probe) return std::unexpected(admission_probe.error());
    bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(
            admission_probe->peak_admitted_bytes, &admission_failure);
    if (!permit) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache seed admission denied before manifest read: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(
                admission_failure.reason),
            " requested=", admission_failure.requested_bytes,
            " available=",
            admission_failure.allocatable_after_reservations_bytes));
    }
    auto manifest_bytes = read_regular_file(
        manifest_path, "sealed shader cache manifest", maximum_manifest_bytes,
        admission_probe->file_bytes);
    if (!manifest_bytes) return std::unexpected(manifest_bytes.error());
    auto manifest = parse_manifest(*manifest_bytes);
    if (!manifest) return std::unexpected(manifest.error());
    if (manifest->seed_size > maximum_seed_image_bytes
        || manifest->seed_size
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())
        || manifest->artifacts.size() > maximum_seed_image_artifacts) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache process image exceeds bounded ownership: bytes=",
            manifest->seed_size, " artifacts=", manifest->artifacts.size()));
    }
    if (manifest->seed_size != admission_probe->seed_bytes
        || manifest->keys.size() != admission_probe->key_count
        || manifest->artifacts.size() != admission_probe->artifact_count) {
        return std::unexpected(
            "sealed shader cache manifest changed after admission probe");
    }
    if (auto valid = validate_shape(
            cache_dir, *manifest, cache_digest, expected_keys);
        !valid) {
        return std::unexpected(valid.error());
    }

    const std::uint64_t peak_admitted_bytes =
        seed_image_preallocation_peak_bytes(
            *manifest,
            static_cast<std::uint64_t>(manifest_bytes->size()));
    if (peak_admitted_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || peak_admitted_bytes > permit->bytes()) {
        return std::unexpected(
            "sealed shader cache seed admission probe underestimated ownership");
    }

    std::vector<SealedShaderCacheArtifactImage> images;
    images.reserve(manifest->artifacts.size());
    std::uint64_t artifact_bytes = 0u;
    for (const SealedShaderCacheArtifact& artifact : manifest->artifacts) {
        auto bytes = load_validated_artifact_bytes(cache_dir, artifact);
        if (!bytes) return std::unexpected(bytes.error());
        if (bytes->size()
            > (std::numeric_limits<std::uint64_t>::max)() - artifact_bytes) {
            return std::unexpected(
                "sealed shader cache seed loaded byte count overflow");
        }
        artifact_bytes += static_cast<std::uint64_t>(bytes->size());
        images.push_back(SealedShaderCacheArtifactImage{
            .descriptor = artifact,
            .bytes = std::move(*bytes),
        });
    }
    if (artifact_bytes != manifest->seed_size) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache seed loaded byte count mismatch: actual=",
            artifact_bytes, " manifest=", manifest->seed_size));
    }
    if (auto valid = validate_all_artifact_images(
            cache_dir, *manifest, images);
        !valid) {
        return std::unexpected(valid.error());
    }
    const auto finished = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        finished - started).count();
    if (elapsed <= 0) {
        return std::unexpected(
            "sealed shader cache seed load produced no positive elapsed time");
    }
    const std::uint64_t manifest_size =
        static_cast<std::uint64_t>(manifest_bytes->size());
    const std::uint64_t artifact_read_count =
        static_cast<std::uint64_t>(images.size());
    const std::uint64_t load_ns = static_cast<std::uint64_t>(elapsed);
    const std::uint64_t receipt_identity = sealed_seed_receipt_identity(
        *manifest, artifact_read_count, manifest_size, artifact_bytes, load_ns);
    SealedShaderCacheSeedImage image{
        std::move(*manifest), std::move(*manifest_bytes), std::move(images),
        receipt_identity, artifact_read_count, manifest_size, artifact_bytes,
        load_ns, admission_probe->peak_admitted_bytes, std::move(*permit)};
    if (!image.reconcile_admission() || !image.accepted()) {
        return std::unexpected(
            "sealed shader cache seed loader produced invalid receipt evidence");
    }
    return std::move(image);
}

/// Install-once process owner for an immutable seed. Both production and
/// focused tests use this exact store, so a second acquisition proves reuse of
/// the original loader receipt rather than a fresh filesystem validation.
class SealedShaderCacheProcessStore {
public:
    [[nodiscard]] std::expected<std::shared_ptr<
        const SealedShaderCacheSeedImage>, std::string>
    load_once(
        const std::filesystem::path& cache_dir,
        std::string_view digest,
        std::span<const ShaderVariantKey> requested_keys)
    {
        auto canonical = canonical_sealed_shader_cache_keys(requested_keys);
        if (!canonical) return std::unexpected(canonical.error());
        std::lock_guard<std::mutex> guard(mutex_);
        const std::filesystem::path normalized = cache_dir.lexically_normal();
        if (image_) {
            if (cache_dir_ != normalized
                || image_->manifest().cache_digest != digest
                || image_->manifest().keys != *canonical) {
                return std::unexpected(
                    "process shader seed is already sealed to different evidence");
            }
            return image_;
        }
        auto loaded = load_validated_sealed_shader_cache_seed(
            cache_dir, digest, *canonical);
        if (!loaded) return std::unexpected(loaded.error());
        auto retained = std::make_shared<const SealedShaderCacheSeedImage>(
            std::move(*loaded));
        if (!retained->accepted()) {
            return std::unexpected(
                "process shader seed has invalid loader-originated evidence");
        }
        cache_dir_ = normalized;
        image_ = retained;
        ++filesystem_load_count_;
        return retained;
    }

    [[nodiscard]] std::shared_ptr<const SealedShaderCacheSeedImage>
    snapshot() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return image_;
    }

    [[nodiscard]] std::uint64_t filesystem_load_count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return filesystem_load_count_;
    }

private:
    mutable std::mutex mutex_;
    std::filesystem::path cache_dir_;
    std::shared_ptr<const SealedShaderCacheSeedImage> image_;
    std::uint64_t filesystem_load_count_ = 0u;
};

namespace sealed_shader_cache_detail {

[[nodiscard]] inline std::expected<SealedShaderCacheManifest, std::string>
seal_shader_cache_manifest_impl(
    const std::filesystem::path& cache_dir,
    std::string_view cache_digest,
    std::span<const ShaderVariantKey> requested_keys,
    SealBeforeIdentityCheckHook before_identity_check)
{
    if (!valid_digest(cache_digest)) {
        return std::unexpected("sealed shader cache digest is not lowercase hex16");
    }
    auto keys = canonical_keys(requested_keys);
    if (!keys) return std::unexpected(keys.error());
    auto expected = expected_artifacts(cache_dir, *keys);
    if (!expected) return std::unexpected(expected.error());
    if (expected->size() > maximum_seed_image_artifacts) {
        return std::unexpected(shader_cache_message(
            "cannot seal shader cache seed beyond artifact cap: artifacts=",
            expected->size()));
    }

    const auto artifact_file_size = [](const ExpectedArtifact& artifact)
        -> std::expected<std::uint64_t, std::string> {
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(artifact.path, error);
        if (error || std::filesystem::is_symlink(status)
            || !std::filesystem::is_regular_file(status)) {
            return std::unexpected(shader_cache_message(
                "cannot seal non-regular shader artifact ",
                artifact.path.string(), error ? ": " + error.message() : ""));
        }
        const std::uintmax_t size =
            std::filesystem::file_size(artifact.path, error);
        if (error || size == 0u || size > maximum_seed_artifact_bytes
            || size > (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(shader_cache_message(
                "cannot seal invalid shader artifact size ",
                artifact.path.string(), ": bytes=", size));
        }
        return static_cast<std::uint64_t>(size);
    };

    std::uint64_t preflight_seed_bytes = 0u;
    std::uint64_t preflight_max_artifact = 0u;
    for (const ExpectedArtifact& artifact : *expected) {
        auto size = artifact_file_size(artifact);
        if (!size) return std::unexpected(size.error());
        if (*size > maximum_seed_image_bytes - preflight_seed_bytes) {
            return std::unexpected(shader_cache_message(
                "cannot seal shader cache seed beyond byte cap: bytes>",
                maximum_seed_image_bytes));
        }
        preflight_seed_bytes += *size;
        preflight_max_artifact = std::max(preflight_max_artifact, *size);
    }
    const std::uint64_t estimated_manifest_bytes =
        bioseq::parallelism::saturating_add(
            256u + static_cast<std::uint64_t>(keys->size())
                    * sizeof(ShaderVariantKey),
            bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(expected->size()),
                56u + maximum_manifest_filename_bytes));
    if (estimated_manifest_bytes > maximum_manifest_bytes) {
        return std::unexpected(
            "cannot seal shader cache seed with oversized manifest");
    }
    const std::uint64_t peak_admitted_bytes = seed_image_probe_peak_bytes(
        estimated_manifest_bytes, preflight_seed_bytes,
        static_cast<std::uint32_t>(keys->size()),
        static_cast<std::uint32_t>(expected->size()));
    if (peak_admitted_bytes
        == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(
            "cannot seal shader cache seed because admission saturated");
    }
    bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(peak_admitted_bytes, &admission_failure);
    if (!permit) {
        return std::unexpected(shader_cache_message(
            "cannot seal shader cache seed because admission was denied: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(
                admission_failure.reason),
            " requested=", admission_failure.requested_bytes));
    }

    std::vector<std::uint64_t> exact_sizes;
    exact_sizes.reserve(expected->size());
    std::uint64_t confirmed_seed_bytes = 0u;
    std::uint64_t confirmed_max_artifact = 0u;
    for (const ExpectedArtifact& artifact : *expected) {
        auto size = artifact_file_size(artifact);
        if (!size) return std::unexpected(size.error());
        if (*size > maximum_seed_image_bytes - confirmed_seed_bytes) {
            return std::unexpected(
                "shader cache seed grew beyond cap during seal admission");
        }
        confirmed_seed_bytes += *size;
        confirmed_max_artifact = std::max(confirmed_max_artifact, *size);
        exact_sizes.push_back(*size);
    }
    if (confirmed_seed_bytes != preflight_seed_bytes
        || confirmed_max_artifact != preflight_max_artifact) {
        return std::unexpected(
            "shader cache seed artifact sizes changed during seal admission");
    }

    SealedShaderCacheManifest manifest{};
    manifest.cache_digest = std::string(cache_digest);
    manifest.keys = *keys;
    manifest.artifacts.reserve(expected->size());
    std::vector<SealedShaderCacheArtifactImage> images;
    images.reserve(expected->size());
    std::vector<RegularFileIdentity> stable_identities;
    stable_identities.reserve(expected->size());
    for (std::size_t ordinal = 0u; ordinal < expected->size(); ++ordinal) {
        const ExpectedArtifact& artifact = (*expected)[ordinal];
        auto stable = read_stable_regular_file(
            artifact.path,
            "shader cache seed artifact",
            exact_sizes[ordinal],
            maximum_seed_artifact_bytes);
        if (!stable) return std::unexpected(stable.error());
        SealedShaderCacheArtifact descriptor{
            .kind = artifact.kind,
            .key_index = artifact.key_index,
            .filename = artifact.filename,
            .size = static_cast<std::uint64_t>(stable->bytes.size()),
            .sha256 = sha256(stable->bytes),
        };
        manifest.artifacts.push_back(descriptor);
        images.push_back(SealedShaderCacheArtifactImage{
            .descriptor = std::move(descriptor),
            .bytes = std::move(stable->bytes),
        });
        stable_identities.push_back(stable->identity);
    }
    auto seed = seed_identity(
        manifest.cache_digest, manifest.keys, manifest.artifacts);
    if (!seed) return std::unexpected(seed.error());
    manifest.seed_sha256 = seed->first;
    manifest.seed_size = seed->second;
    if (manifest.seed_size != confirmed_seed_bytes
        || manifest.seed_size > maximum_seed_image_bytes
        || manifest.artifacts.size() > maximum_seed_image_artifacts) {
        return std::unexpected(
            "shader cache seed candidate exceeds admitted publication bounds");
    }
    if (auto shape = validate_shape(
            cache_dir, manifest, cache_digest, *keys);
        !shape) {
        return std::unexpected(shape.error());
    }
    if (auto valid = validate_all_artifact_images(
            cache_dir, manifest, images);
        !valid) {
        return std::unexpected(valid.error());
    }
    auto serialized = serialize_manifest(manifest);
    if (!serialized) return std::unexpected(serialized.error());
    manifest.manifest_checksum = serialized->second;
    auto parsed = parse_manifest(serialized->first);
    if (!parsed) return std::unexpected(parsed.error());
    if (auto shape = validate_shape(
            cache_dir, *parsed, cache_digest, *keys);
        !shape) {
        return std::unexpected(shape.error());
    }
    if (auto valid = validate_all_artifact_images(
            cache_dir, *parsed, images);
        !valid) {
        return std::unexpected(valid.error());
    }
    if (before_identity_check != nullptr) before_identity_check();
    for (std::size_t ordinal = 0u; ordinal < expected->size(); ++ordinal) {
        if (!same_regular_file_identity(
                (*expected)[ordinal].path, stable_identities[ordinal])) {
            return std::unexpected(shader_cache_message(
                "shader cache seed artifact identity changed before publication: ",
                (*expected)[ordinal].filename));
        }
    }
    if (auto written = durable_write_file(
            sealed_shader_cache_manifest_path(cache_dir), serialized->first);
        !written) {
        return std::unexpected(written.error());
    }
    return parsed;
}

}  // namespace sealed_shader_cache_detail

[[nodiscard]] inline std::expected<SealedShaderCacheManifest, std::string>
seal_shader_cache_manifest(
    const std::filesystem::path& cache_dir,
    std::string_view cache_digest,
    std::span<const ShaderVariantKey> requested_keys)
{
    return sealed_shader_cache_detail::seal_shader_cache_manifest_impl(
        cache_dir, cache_digest, requested_keys, nullptr);
}

[[nodiscard]] inline std::expected<SealedShaderCacheManifest, std::string>
seal_shader_cache_manifest_for_test(
    const std::filesystem::path& cache_dir,
    std::string_view cache_digest,
    std::span<const ShaderVariantKey> requested_keys,
    sealed_shader_cache_detail::SealBeforeIdentityCheckHook before_identity_check)
{
    return sealed_shader_cache_detail::seal_shader_cache_manifest_impl(
        cache_dir, cache_digest, requested_keys, before_identity_check);
}

[[nodiscard]] inline std::expected<SealedShaderCacheManifest, std::string>
load_validated_sealed_shader_cache_manifest(
    const std::filesystem::path& cache_dir,
    std::string_view cache_digest,
    std::span<const ShaderVariantKey> expected_keys)
{
    auto seed = load_validated_sealed_shader_cache_seed(
        cache_dir, cache_digest, expected_keys);
    if (!seed) return std::unexpected(seed.error());
    return seed->manifest();
}

[[nodiscard]] inline std::expected<SealedShaderCacheKeyView, std::string>
sealed_shader_cache_key_view(
    const SealedShaderCacheSeedImage& seed,
    const ShaderVariantKey& requested_key)
{
    if (!seed.accepted()) {
        return std::unexpected(
            "sealed shader cache process image has invalid loader receipt");
    }
    const SealedShaderCacheManifest& manifest = seed.manifest();
    const auto key = std::find(
        manifest.keys.begin(), manifest.keys.end(), requested_key);
    if (key == manifest.keys.end()) {
        return std::unexpected(shader_cache_message(
            "requested shader is not a member of the sealed AOT seed: ",
            cache_file_stem(requested_key)));
    }
    const std::uint32_t key_index = static_cast<std::uint32_t>(
        std::distance(manifest.keys.begin(), key));
    const std::string stem = cache_file_stem(requested_key);
    const auto* metadata = sealed_shader_cache_detail::find_artifact_image(
        seed.artifacts(), SealedShaderCacheArtifactKind::shader_meta,
        key_index, stem + ".meta");
    const auto* metallib = sealed_shader_cache_detail::find_artifact_image(
        seed.artifacts(), SealedShaderCacheArtifactKind::shader_metallib,
        key_index, stem + ".metallib");
    if (metadata == nullptr || metallib == nullptr
        || metallib->bytes.empty()) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache key view incomplete for ", stem));
    }
    SealedShaderCacheKeyView view{};
    view.key = requested_key;
    auto meta = compatible_cache_meta_from_bytes(
        metadata->bytes, requested_key, &view.entry_name);
    if (!meta) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache metadata incompatible for ", stem,
            ": ", meta.error()));
    }
    view.meta = *meta;
    view.metallib_bytes = metallib->bytes;
    return view;
}

[[nodiscard]] inline std::expected<SealedShaderCacheKeyImage, std::string>
load_sealed_shader_cache_key_image(
    const std::filesystem::path& cache_dir,
    std::string_view cache_digest,
    std::span<const ShaderVariantKey> expected_keys,
    const ShaderVariantKey& requested_key)
{
    auto seed = load_validated_sealed_shader_cache_seed(
        cache_dir, cache_digest, expected_keys);
    if (!seed) return std::unexpected(seed.error());
    const SealedShaderCacheManifest& manifest = seed->manifest();
    const auto key = std::find(
        manifest.keys.begin(), manifest.keys.end(), requested_key);
    if (key == manifest.keys.end()) {
        return std::unexpected(shader_cache_message(
            "requested shader is not a member of the sealed AOT seed: ",
            cache_file_stem(requested_key)));
    }
    const std::uint32_t key_index = static_cast<std::uint32_t>(
        std::distance(manifest.keys.begin(), key));
    SealedShaderCacheKeyImage image{};
    image.manifest = manifest;
    image.key = requested_key;
    bool metadata_loaded = false;
    bool metallib_loaded = false;
    for (const SealedShaderCacheArtifactImage& artifact : seed->artifacts()) {
        if (artifact.descriptor.key_index != key_index) {
            continue;
        }
        switch (artifact.descriptor.kind) {
        case SealedShaderCacheArtifactKind::shader_meta: {
            auto meta = compatible_cache_meta_from_bytes(
                artifact.bytes, requested_key, &image.entry_name);
            if (!meta) {
                return std::unexpected(shader_cache_message(
                    "sealed shader cache metadata incompatible for ",
                    cache_file_stem(requested_key), ": ", meta.error()));
            }
            image.meta = *meta;
            metadata_loaded = true;
            break;
        }
        case SealedShaderCacheArtifactKind::shader_metallib:
            image.metallib_bytes = artifact.bytes;
            metallib_loaded = true;
            break;
        case SealedShaderCacheArtifactKind::metal_binary_archive:
        case SealedShaderCacheArtifactKind::metal_binary_archive_qualification:
            break;
        case SealedShaderCacheArtifactKind::reserved_command_metallib:
            return std::unexpected(
                "sealed shader cache contains forbidden placeholder command artifact");
        }
    }
    if (!metadata_loaded || !metallib_loaded || image.metallib_bytes.empty()) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache key image incomplete for ",
            cache_file_stem(requested_key)));
    }
    return image;
}

[[nodiscard]] inline std::expected<SealedShaderCacheManifest, std::string>
load_sealed_shader_cache_manifest_for_key(
    const std::filesystem::path& cache_dir,
    std::string_view cache_digest,
    std::span<const ShaderVariantKey> expected_keys,
    const ShaderVariantKey& requested_key)
{
    auto image = load_sealed_shader_cache_key_image(
        cache_dir, cache_digest, expected_keys, requested_key);
    if (!image) return std::unexpected(image.error());
    return std::move(image->manifest);
}

struct QualifiedMetalArchiveHelper {
    bioseq::parallelism::MemoryPermit permit;
    std::filesystem::path path;
    sealed_shader_cache_detail::RegularFileIdentity identity{};
    std::uint64_t executable_size = 0u;
    std::array<std::uint8_t, 32> executable_sha256{};
    std::vector<std::byte> executable_bytes;
    std::string source;
    std::uint64_t hash_passes = 0u;

    [[nodiscard]] bool accepted() const noexcept {
        return permit && permit.bytes() == owned_capacity_bytes()
            && !path.empty() && executable_size != 0u && hash_passes == 1u
            && executable_bytes.size() == executable_size
            && (source == "installed-sibling"
                || source == "test-env-override")
            && std::any_of(
                executable_sha256.begin(), executable_sha256.end(),
                [](std::uint8_t byte) { return byte != 0u; });
    }

    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::parallelism::saturating_add;
        std::uint64_t owned = sizeof(QualifiedMetalArchiveHelper);
        owned = saturating_add(
            owned, static_cast<std::uint64_t>(path.native().capacity()));
        owned = saturating_add(
            owned, static_cast<std::uint64_t>(executable_bytes.capacity()));
        owned = saturating_add(
            owned, static_cast<std::uint64_t>(source.capacity()));
        return owned;
    }
};

[[nodiscard]] inline std::expected<QualifiedMetalArchiveHelper, std::string>
load_qualified_metal_archive_helper()
{
#if !defined(__APPLE__)
    return std::unexpected(
        "Metal archive helper qualification is unavailable off Apple platforms");
#else
    std::filesystem::path path;
    std::string source;
    if (const char* env = std::getenv("BIOSEQ_GPU_ARCHIVE_HELPER");
        env != nullptr && *env != '\0') {
        const char* test_only =
            std::getenv("BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY");
        if (test_only == nullptr || std::string_view{test_only} != "1") {
            return std::unexpected(
                "BIOSEQ_GPU_ARCHIVE_HELPER is rejected outside explicit test-only mode");
        }
        path = env;
        source = "test-env-override";
    } else {
        Dl_info info{};
        if (::dladdr(
                reinterpret_cast<const void*>(
                    &load_qualified_metal_archive_helper),
                &info) == 0
            || info.dli_fname == nullptr) {
            return std::unexpected(
                "cannot locate bioseq.dylib for installed archive helper qualification");
        }
        const std::filesystem::path dylib_path(info.dli_fname);
        path = dylib_path.parent_path() / "bioseq_gpu_archive_builder";
        source = "installed-sibling";
    }
    path = path.lexically_normal();
    struct stat path_status {};
    if (::lstat(path.c_str(), &path_status) != 0
        || !S_ISREG(path_status.st_mode) || S_ISLNK(path_status.st_mode)
        || (path_status.st_mode & (S_IWGRP | S_IWOTH)) != 0
        || (path_status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0
        || ::access(path.c_str(), X_OK) != 0) {
        return std::unexpected(shader_cache_message(
            "archive helper is not a non-symlink executable regular file: ",
            path.string()));
    }
    if (path_status.st_size <= 0
        || static_cast<std::uint64_t>(path_status.st_size)
            > sealed_shader_cache_detail::maximum_seed_artifact_bytes) {
        return std::unexpected("archive helper executable size is invalid");
    }
    const std::uint64_t admission_peak =
        bioseq::parallelism::saturating_add(
            bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(path_status.st_size),
                bioseq::parallelism::kVectorPreallocationHeadroomFactor),
            sizeof(QualifiedMetalArchiveHelper) + 4096u);
    bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(admission_peak, &admission_failure);
    if (!permit) {
        return std::unexpected(
            "archive helper executable memory admission denied");
    }
    auto image = sealed_shader_cache_detail::read_stable_regular_file(
        path, "qualified Metal archive helper",
        static_cast<std::uint64_t>(path_status.st_size),
        sealed_shader_cache_detail::maximum_seed_artifact_bytes);
    if (!image) return std::unexpected(image.error());
    struct stat after_status {};
    if (::lstat(path.c_str(), &after_status) != 0
        || !S_ISREG(after_status.st_mode) || S_ISLNK(after_status.st_mode)
        || (after_status.st_mode & (S_IWGRP | S_IWOTH)) != 0
        || (after_status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0
        || ::access(path.c_str(), X_OK) != 0
        || sealed_shader_cache_detail::regular_file_identity(after_status)
            != image->identity) {
        return std::unexpected(
            "archive helper identity changed during process qualification");
    }
    QualifiedMetalArchiveHelper helper{
        .permit = std::move(*permit),
        .path = std::move(path),
        .identity = image->identity,
        .executable_size = static_cast<std::uint64_t>(image->bytes.size()),
        .executable_sha256 =
            sealed_shader_cache_detail::sha256(image->bytes),
        .executable_bytes = std::move(image->bytes),
        .source = std::move(source),
        .hash_passes = 1u,
    };
    const std::uint64_t retained = helper.owned_capacity_bytes();
    if (retained == (std::numeric_limits<std::uint64_t>::max)()
        || retained > helper.permit.bytes()) {
        return std::unexpected(
            "archive helper executable ownership admission saturated");
    }
    helper.permit.shrink_to(retained);
    if (!helper.accepted()) {
        return std::unexpected("archive helper qualification is incomplete");
    }
    return helper;
#endif
}

class QualifiedMetalArchiveHelperProcessStore {
public:
    [[nodiscard]] std::expected<
        std::shared_ptr<const QualifiedMetalArchiveHelper>, std::string>
    load_once()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (result_) return *result_;
        ++qualification_attempt_count_;
        auto loaded = load_qualified_metal_archive_helper();
        if (!loaded) {
            result_.emplace(std::unexpected(loaded.error()));
            return *result_;
        }
        filesystem_hash_count_ += loaded->hash_passes;
        auto retained =
            std::make_shared<const QualifiedMetalArchiveHelper>(
                std::move(*loaded));
        result_.emplace(std::move(retained));
        return *result_;
    }

    [[nodiscard]] std::uint64_t filesystem_hash_count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return filesystem_hash_count_;
    }

    [[nodiscard]] std::uint64_t qualification_attempt_count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return qualification_attempt_count_;
    }

    [[nodiscard]] bool reset_for_test() noexcept {
        const char* enabled =
            std::getenv("BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY");
        if (enabled == nullptr || std::string_view{enabled} != "1") {
            return false;
        }
        std::lock_guard<std::mutex> guard(mutex_);
        result_.reset();
        filesystem_hash_count_ = 0u;
        qualification_attempt_count_ = 0u;
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::optional<std::expected<
        std::shared_ptr<const QualifiedMetalArchiveHelper>, std::string>>
        result_;
    std::uint64_t filesystem_hash_count_ = 0u;
    std::uint64_t qualification_attempt_count_ = 0u;
};

[[nodiscard]] inline QualifiedMetalArchiveHelperProcessStore&
qualified_metal_archive_helper_process_store() noexcept
{
    static QualifiedMetalArchiveHelperProcessStore store;
    return store;
}

[[nodiscard]] inline std::expected<void, std::string>
validate_metal_archive_receipt_against_process_helper(
    const MetalArchiveQualificationReceipt& receipt)
{
    if (!receipt.production_accepted()) {
        return std::unexpected(
            "archive receipt is not production qualification v2");
    }
    auto helper = qualified_metal_archive_helper_process_store().load_once();
    if (!helper) return std::unexpected(helper.error());
    const QualifiedMetalArchiveHelper& current = **helper;
    if (current.source != receipt.helper_source
        || current.executable_size != receipt.helper_executable_size
        || current.executable_sha256 != receipt.helper_executable_sha256
        || current.hash_passes != 1u
        || qualified_metal_archive_helper_process_store()
                .filesystem_hash_count() != 1u
        || qualified_metal_archive_helper_process_store()
                .qualification_attempt_count() != 1u) {
        return std::unexpected(
            "archive receipt does not match current once-qualified helper executable");
    }
    return {};
}

[[nodiscard]] inline std::expected<std::filesystem::path, std::string>
metal_archive_helper_path()
{
    auto helper = qualified_metal_archive_helper_process_store().load_once();
    if (!helper) return std::unexpected(helper.error());
    return (*helper)->path;
}

[[nodiscard]] inline bool archive_helper_transcript_is_exact_success(
    std::string_view transcript) noexcept
{
    constexpr std::string_view success_line = "RESULT: ok\n";
    const std::size_t result_marker = transcript.find("RESULT:");
    return transcript.size() >= success_line.size()
        && transcript.substr(transcript.size() - success_line.size())
            == success_line
        && result_marker == transcript.size() - success_line.size();
}

[[nodiscard]] inline std::expected<MetalArchiveNegativeErrorFingerprint, std::string>
parse_archive_helper_negative_error_fingerprint(
    std::string_view transcript)
{
    constexpr std::string_view prefix =
        "NEGATIVE_CONTROL_NSError domain_hex=";
    constexpr std::string_view code_separator = " code=";
    const std::size_t marker = transcript.find(prefix);
    if (marker == std::string_view::npos
        || transcript.find(prefix, marker + prefix.size())
            != std::string_view::npos
        || (marker != 0u && transcript[marker - 1u] != '\n')) {
        return std::unexpected(
            "archive helper negative-control fingerprint is missing or repeated");
    }
    const std::size_t line_end = transcript.find('\n', marker);
    if (line_end == std::string_view::npos) {
        return std::unexpected(
            "archive helper negative-control fingerprint line is unterminated");
    }
    const std::string_view payload = transcript.substr(
        marker + prefix.size(), line_end - marker - prefix.size());
    const std::size_t code_marker = payload.find(code_separator);
    if (code_marker == std::string_view::npos
        || payload.find(code_separator, code_marker + code_separator.size())
            != std::string_view::npos) {
        return std::unexpected(
            "archive helper negative-control fingerprint fields are invalid");
    }
    const std::string_view domain_hex = payload.substr(0u, code_marker);
    if (domain_hex.empty() || domain_hex.size() > 2048u
        || (domain_hex.size() & 1u) != 0u) {
        return std::unexpected(
            "archive helper negative-control NSError domain hex is invalid");
    }
    std::string domain;
    domain.reserve(domain_hex.size() / 2u);
    const auto hex_digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0u; index < domain_hex.size(); index += 2u) {
        const int high = hex_digit(domain_hex[index]);
        const int low = hex_digit(domain_hex[index + 1u]);
        if (high < 0 || low < 0) {
            return std::unexpected(
                "archive helper negative-control NSError domain is not lowercase hex");
        }
        domain.push_back(static_cast<char>((high << 4) | low));
    }
    const std::string_view code_text = payload.substr(
        code_marker + code_separator.size());
    std::int64_t code = 0;
    const auto [end, error] = std::from_chars(
        code_text.data(), code_text.data() + code_text.size(), code);
    if (error != std::errc{} || end != code_text.data() + code_text.size()) {
        return std::unexpected(
            "archive helper negative-control NSError code is invalid");
    }
    MetalArchiveNegativeErrorFingerprint fingerprint{
        .domain = std::move(domain),
        .code = code,
    };
    if (!fingerprint.accepted()) {
        return std::unexpected(
            "archive helper negative-control NSError fingerprint is empty");
    }
    return fingerprint;
}

struct ArchiveHelperQualificationV2 {
    std::uint32_t protocol_version = 0u;
    std::string build_identity;
    std::uint64_t metal_registry_id = 0u;
};

[[nodiscard]] inline std::expected<ArchiveHelperQualificationV2, std::string>
parse_archive_helper_qualification_v2(std::string_view transcript)
{
    constexpr std::string_view prefix =
        "HELPER_QUALIFICATION_V2 protocol=";
    constexpr std::string_view build_separator = " build=";
    constexpr std::string_view registry_separator = " registry_id=";
    const std::size_t marker = transcript.find(prefix);
    if (marker == std::string_view::npos
        || transcript.find(prefix, marker + prefix.size())
            != std::string_view::npos
        || (marker != 0u && transcript[marker - 1u] != '\n')) {
        return std::unexpected(
            "archive helper v2 qualification line is missing or repeated");
    }
    const std::size_t line_end = transcript.find('\n', marker);
    if (line_end == std::string_view::npos) {
        return std::unexpected(
            "archive helper v2 qualification line is unterminated");
    }
    const std::string_view payload = transcript.substr(
        marker + prefix.size(), line_end - marker - prefix.size());
    const std::size_t build_marker = payload.find(build_separator);
    const std::size_t registry_marker = payload.find(registry_separator);
    if (build_marker == std::string_view::npos
        || registry_marker == std::string_view::npos
        || build_marker >= registry_marker
        || payload.find(build_separator, build_marker + 1u)
            != std::string_view::npos
        || payload.find(registry_separator, registry_marker + 1u)
            != std::string_view::npos) {
        return std::unexpected(
            "archive helper v2 qualification fields are invalid");
    }
    const std::string_view protocol_text = payload.substr(0u, build_marker);
    const std::string_view build = payload.substr(
        build_marker + build_separator.size(),
        registry_marker - build_marker - build_separator.size());
    const std::string_view registry_text = payload.substr(
        registry_marker + registry_separator.size());
    std::uint32_t protocol = 0u;
    std::uint64_t registry = 0u;
    const auto [protocol_end, protocol_error] = std::from_chars(
        protocol_text.data(), protocol_text.data() + protocol_text.size(),
        protocol);
    const auto [registry_end, registry_error] = std::from_chars(
        registry_text.data(), registry_text.data() + registry_text.size(),
        registry);
    if (protocol_error != std::errc{}
        || protocol_end != protocol_text.data() + protocol_text.size()
        || registry_error != std::errc{}
        || registry_end != registry_text.data() + registry_text.size()
        || protocol != 2u || registry == 0u
        || build != "bioseq_gpu_archive_builder_v2") {
        return std::unexpected(
            "archive helper v2 protocol, build, or registry identity is invalid");
    }
    return ArchiveHelperQualificationV2{
        .protocol_version = protocol,
        .build_identity = std::string(build),
        .metal_registry_id = registry,
    };
}

inline constexpr int archive_helper_metallib_child_fd = 197;

[[nodiscard]] inline std::expected<void, std::string>
write_and_verify_exact_fd(int fd, std::span<const std::byte> bytes)
{
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(
            fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            return std::unexpected(
                "private helper staging write failed");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
        return std::unexpected(
            "private helper staging sync/rewind failed");
    }
    std::array<std::byte, 64u << 10u> scratch{};
    std::size_t verified = 0u;
    for (;;) {
        const ssize_t count = ::read(fd, scratch.data(), scratch.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            return std::unexpected(
                "private helper staging verification read failed");
        }
        if (count == 0) break;
        const std::size_t chunk = static_cast<std::size_t>(count);
        if (verified > bytes.size() || chunk > bytes.size() - verified
            || !std::equal(
                scratch.begin(),
                scratch.begin() + static_cast<std::ptrdiff_t>(chunk),
                bytes.begin() + static_cast<std::ptrdiff_t>(verified))) {
            return std::unexpected(
                "private helper staging bytes differ from qualified image");
        }
        verified += chunk;
    }
    if (verified != bytes.size() || ::lseek(fd, 0, SEEK_SET) < 0) {
        return std::unexpected(
            "private helper staging verification is incomplete");
    }
    return {};
}

class StagedArchiveHelperInvocation {
public:
    StagedArchiveHelperInvocation() = default;
    StagedArchiveHelperInvocation(const StagedArchiveHelperInvocation&) = delete;
    StagedArchiveHelperInvocation& operator=(
        const StagedArchiveHelperInvocation&) = delete;
    StagedArchiveHelperInvocation(
        StagedArchiveHelperInvocation&& other) noexcept
        : directory_(std::exchange(other.directory_, {})),
          helper_path_(std::exchange(other.helper_path_, {})),
          helper_fd_(std::exchange(other.helper_fd_, -1)),
          metallib_fd_(std::exchange(other.metallib_fd_, -1)),
          helper_identity_(other.helper_identity_),
          helper_linked_(std::exchange(other.helper_linked_, false)) {}
    ~StagedArchiveHelperInvocation() noexcept { cleanup(); }

    [[nodiscard]] const std::filesystem::path& helper_path() const noexcept {
        return helper_path_;
    }
    [[nodiscard]] int metallib_fd() const noexcept { return metallib_fd_; }

    [[nodiscard]] bool helper_identity_matches() const noexcept {
        if (helper_fd_ < 0 || helper_path_.empty() || !helper_linked_) {
            return false;
        }
        struct stat fd_status {};
        struct stat path_status {};
        return ::fstat(helper_fd_, &fd_status) == 0
            && ::lstat(helper_path_.c_str(), &path_status) == 0
            && S_ISREG(fd_status.st_mode) && S_ISREG(path_status.st_mode)
            && !S_ISLNK(path_status.st_mode)
            && sealed_shader_cache_detail::regular_file_identity(fd_status)
                == helper_identity_
            && sealed_shader_cache_detail::regular_file_identity(path_status)
                == helper_identity_;
    }

    [[nodiscard]] std::expected<void, std::string> finish()
    {
        if (!helper_identity_matches()) {
            return std::unexpected(
                "private archive helper identity changed across execution");
        }
        if (::unlink(helper_path_.c_str()) != 0) {
            return std::unexpected(
                "private archive helper could not be unlinked");
        }
        helper_linked_ = false;
        struct stat absent {};
        errno = 0;
        if (::lstat(helper_path_.c_str(), &absent) == 0 || errno != ENOENT) {
            return std::unexpected(
                "private archive helper name remained after unlink");
        }
        if (::close(std::exchange(helper_fd_, -1)) != 0
            || ::close(std::exchange(metallib_fd_, -1)) != 0) {
            return std::unexpected(
                "private archive helper descriptors did not close");
        }
        helper_path_.clear();
        if (::rmdir(directory_.c_str()) != 0) {
            return std::unexpected(
                "private archive helper directory did not disappear");
        }
        directory_.clear();
        return {};
    }

private:
    void cleanup() noexcept {
        if (helper_linked_ && !helper_path_.empty()) {
            (void)::unlink(helper_path_.c_str());
        }
        if (helper_fd_ >= 0) (void)::close(helper_fd_);
        if (metallib_fd_ >= 0) (void)::close(metallib_fd_);
        if (!directory_.empty()) (void)::rmdir(directory_.c_str());
        helper_linked_ = false;
        helper_fd_ = -1;
        metallib_fd_ = -1;
    }

    std::filesystem::path directory_;
    std::filesystem::path helper_path_;
    int helper_fd_ = -1;
    int metallib_fd_ = -1;
    sealed_shader_cache_detail::RegularFileIdentity helper_identity_{};
    bool helper_linked_ = false;

    friend std::expected<StagedArchiveHelperInvocation, std::string>
    stage_archive_helper_invocation(
        const QualifiedMetalArchiveHelper&,
        int);
};

[[nodiscard]] inline std::expected<StagedArchiveHelperInvocation, std::string>
stage_archive_helper_invocation(
    const QualifiedMetalArchiveHelper& helper,
    int metallib_fd)
{
    if (!helper.accepted() || metallib_fd < 0) {
        return std::unexpected(
            "private archive helper staging inputs are incomplete");
    }
    std::error_code temp_error;
    const std::filesystem::path temp_parent =
        std::filesystem::temp_directory_path(temp_error);
    std::error_code canonical_error;
    const std::filesystem::path canonical_parent = temp_error
        ? std::filesystem::path{}
        : std::filesystem::canonical(temp_parent, canonical_error);
    if (temp_error || canonical_error || !canonical_parent.is_absolute()) {
        return std::unexpected(
            "private archive helper staging parent is invalid");
    }
    std::string pattern =
        (canonical_parent / ".bioseq-archive-helper-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* directory = ::mkdtemp(writable.data());
    if (directory == nullptr) {
        return std::unexpected(
            "private archive helper directory creation failed");
    }
    StagedArchiveHelperInvocation staged;
    staged.directory_ = directory;
    if (::chmod(staged.directory_.c_str(), S_IRWXU) != 0) {
        return std::unexpected(
            "private archive helper directory restriction failed");
    }
    struct stat directory_status {};
    if (::lstat(staged.directory_.c_str(), &directory_status) != 0
        || !S_ISDIR(directory_status.st_mode)
        || directory_status.st_uid != ::geteuid()
        || (directory_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(
            "private archive helper directory identity is invalid");
    }
    staged.helper_path_ = staged.directory_ / "helper";
    staged.helper_fd_ = ::open(
        staged.helper_path_.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRWXU);
    if (staged.helper_fd_ < 0) {
        return std::unexpected(
            "private archive helper executable creation failed");
    }
    staged.helper_linked_ = true;
    if (auto exact = write_and_verify_exact_fd(
            staged.helper_fd_, helper.executable_bytes);
        !exact) {
        return std::unexpected(exact.error());
    }
    if (::fchmod(staged.helper_fd_, S_IRUSR | S_IXUSR) != 0) {
        return std::unexpected(
            "private archive helper executable mode failed");
    }
    struct stat helper_status {};
    if (::fstat(staged.helper_fd_, &helper_status) != 0
        || !S_ISREG(helper_status.st_mode)
        || helper_status.st_uid != ::geteuid()
        || (helper_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(
            "private archive helper executable identity is invalid");
    }
    staged.helper_identity_ =
        sealed_shader_cache_detail::regular_file_identity(helper_status);
    // Do not carry a writable executable descriptor across posix_spawn.
    // Darwin may reject execution while the vnode is still open for writing,
    // and the retained proof should hold only a non-mutating descriptor.
    if (::close(std::exchange(staged.helper_fd_, -1)) != 0) {
        return std::unexpected(
            "private archive helper writable descriptor did not close");
    }
    staged.helper_fd_ = ::open(
        staged.helper_path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat reopened_helper_status {};
    if (staged.helper_fd_ < 0
        || ::fstat(staged.helper_fd_, &reopened_helper_status) != 0
        || !S_ISREG(reopened_helper_status.st_mode)
        || sealed_shader_cache_detail::regular_file_identity(
               reopened_helper_status)
            != staged.helper_identity_) {
        return std::unexpected(
            "private archive helper read-only reopen changed identity");
    }

    if (::lseek(metallib_fd, 0, SEEK_SET) < 0) {
        return std::unexpected(
            "qualified metallib descriptor rewind failed");
    }
    staged.metallib_fd_ =
        ::fcntl(
            metallib_fd,
            F_DUPFD_CLOEXEC,
            archive_helper_metallib_child_fd + 1);
    struct stat source_metallib {};
    struct stat inherited_metallib {};
    if (staged.metallib_fd_ < 0
        || ::fstat(metallib_fd, &source_metallib) != 0
        || ::fstat(staged.metallib_fd_, &inherited_metallib) != 0
        || !S_ISREG(inherited_metallib.st_mode)
        || sealed_shader_cache_detail::regular_file_identity(
               source_metallib)
            != sealed_shader_cache_detail::regular_file_identity(
                inherited_metallib)
        || !staged.helper_identity_matches()) {
        return std::unexpected(
            "private archive staging identity changed before execution");
    }
    return staged;
}

[[nodiscard]] inline std::expected<void, std::string> ensure_binary_archive(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key,
    const ShaderCacheMeta& meta,
    std::string_view entry_name,
    bool force_requalify = false)
{
#if !defined(__APPLE__)
    (void)cache_dir;
    (void)key;
    (void)meta;
    (void)entry_name;
    (void)force_requalify;
    return {};
#else
    if (entry_name.empty()) {
        return std::unexpected(shader_cache_message(
            "cannot build MTLBinaryArchive for ", cache_file_stem(key),
            ": empty Metal entry name"));
    }
    std::error_code ec;
    if (force_requalify) {
        for (const std::filesystem::path& final_path :
             cache_binary_archive_paths(cache_dir, key)) {
            std::filesystem::remove(final_path, ec);
            if (ec) {
                return std::unexpected(shader_cache_message(
                    "remove unsealed MTLBinaryArchive before qualification failed for ",
                    cache_file_stem(key), ": ", ec.message()));
            }
            std::filesystem::remove(final_path.string() + ".tmp", ec);
            if (ec) {
                return std::unexpected(shader_cache_message(
                    "remove stale MTLBinaryArchive temporary failed for ",
                    cache_file_stem(key), ": ", ec.message()));
            }
            const std::filesystem::path qualification_path =
                cache_binary_archive_qualification_path(final_path);
            std::filesystem::remove(qualification_path, ec);
            if (ec) {
                return std::unexpected(shader_cache_message(
                    "remove unsealed MTLBinaryArchive qualification failed for ",
                    cache_file_stem(key), ": ", ec.message()));
            }
            std::filesystem::remove(qualification_path.string() + ".tmp", ec);
            if (ec) {
                return std::unexpected(shader_cache_message(
                    "remove stale MTLBinaryArchive qualification temporary failed for ",
                    cache_file_stem(key), ": ", ec.message()));
            }
        }
    }
    const auto helper =
        qualified_metal_archive_helper_process_store().load_once();
    if (!helper) {
        return std::unexpected(helper.error());
    }
    const QualifiedMetalArchiveHelper& qualified_helper = **helper;
    const std::filesystem::path metallib_path =
        cache_metallib_path(cache_dir, key);
    const int metallib_fd = ::open(
        metallib_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (metallib_fd < 0) {
        return std::unexpected(
            "qualified Metal shader library open failed");
    }
    CaptureFileDescriptor metallib_fd_owner{metallib_fd};
    struct stat metallib_before {};
    if (::fstat(metallib_fd, &metallib_before) != 0
        || !S_ISREG(metallib_before.st_mode)
        || metallib_before.st_size <= 0
        || static_cast<std::uint64_t>(metallib_before.st_size)
            > sealed_shader_cache_detail::maximum_seed_artifact_bytes) {
        return std::unexpected(
            "qualified Metal shader library identity is invalid");
    }
    sealed_shader_cache_detail::Sha256 metallib_hasher;
    std::array<std::byte, 64u << 10u> metallib_scratch{};
    std::uint64_t metallib_size = 0u;
    for (;;) {
        const ssize_t count = ::read(
            metallib_fd, metallib_scratch.data(), metallib_scratch.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            return std::unexpected(
                "qualified Metal shader library hash read failed");
        }
        if (count == 0) break;
        const std::size_t chunk = static_cast<std::size_t>(count);
        metallib_hasher.update(std::span<const std::byte>{
            metallib_scratch.data(), chunk});
        metallib_size += static_cast<std::uint64_t>(chunk);
    }
    struct stat metallib_after {};
    if (::fstat(metallib_fd, &metallib_after) != 0
        || sealed_shader_cache_detail::regular_file_identity(metallib_before)
            != sealed_shader_cache_detail::regular_file_identity(
                metallib_after)
        || metallib_size != static_cast<std::uint64_t>(metallib_after.st_size)
        || ::lseek(metallib_fd, 0, SEEK_SET) < 0) {
        return std::unexpected(
            "qualified Metal shader library changed during hash");
    }
    const auto metallib_sha256 = metallib_hasher.finalize();

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

    for (std::size_t cap_index = 0; cap_index < archive_thread_caps.size(); ++cap_index) {
        const std::uint64_t max_threads = archive_thread_caps[cap_index];
        const std::filesystem::path final_path = cap_index == 0
            ? cache_binary_archive_path(cache_dir, key)
            : cache_dir / (cache_file_stem(key) + ".max_threads_"
                           + std::to_string(max_threads) + ".metalar");
        const std::filesystem::path qualification_path =
            cache_binary_archive_qualification_path(final_path);
        if (!force_requalify
            && std::filesystem::exists(final_path, ec) && !ec
            && std::filesystem::file_size(final_path, ec) > 0 && !ec
            && std::filesystem::exists(qualification_path, ec) && !ec
            && std::filesystem::file_size(qualification_path, ec) > 0 && !ec) {
            continue;
        }
        ec.clear();
        const std::filesystem::path tmp_path = final_path.string() + ".tmp";
        const std::filesystem::path qualification_tmp =
            qualification_path.string() + ".tmp";
        std::filesystem::remove(tmp_path, ec);
        ec.clear();
        std::filesystem::remove(qualification_tmp, ec);
        ec.clear();

        auto staged_helper = stage_archive_helper_invocation(
            qualified_helper, metallib_fd);
        if (!staged_helper) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return std::unexpected(staged_helper.error());
        }
        const std::vector<std::string> args{
            staged_helper->helper_path().string(),
            "/dev/fd/" + std::to_string(archive_helper_metallib_child_fd),
            tmp_path.string(),
            std::string(entry_name),
            std::to_string(max_threads),
            "0",
        };
        if (!staged_helper->helper_identity_matches()) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return std::unexpected(
                "private archive helper identity changed before execution");
        }
        const std::array inherited_fds{
            RunCaptureInheritedFd{
                .source_fd = staged_helper->metallib_fd(),
                .target_fd = archive_helper_metallib_child_fd,
            },
        };
        auto result = run_capture(
            args,
            RunCaptureLimits{
                .maximum_output_bytes = 64u << 10u,
                .timeout = std::chrono::minutes(2),
            },
            inherited_fds);
        struct stat metallib_after_execution {};
        const bool metallib_identity_unchanged =
            ::fstat(metallib_fd, &metallib_after_execution) == 0
            && sealed_shader_cache_detail::regular_file_identity(
                   metallib_after_execution)
                == sealed_shader_cache_detail::regular_file_identity(
                    metallib_after);
        auto staging_finished = staged_helper->finish();
        if (!staging_finished) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return std::unexpected(staging_finished.error());
        }
        if (!metallib_identity_unchanged) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return std::unexpected(
                "qualified metallib descriptor identity changed during helper execution");
        }
        if (!result) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper failed for ", cache_file_stem(key), ": ",
                result.error()));
        }
        if (!archive_helper_transcript_is_exact_success(*result)) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper returned without exact success sentinel for ",
                cache_file_stem(key), ": ", *result));
        }
        auto negative_error = parse_archive_helper_negative_error_fingerprint(
            *result);
        if (!negative_error) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper returned invalid negative-control evidence for ",
                cache_file_stem(key), ": ", negative_error.error()));
        }
        auto helper_qualification =
            parse_archive_helper_qualification_v2(*result);
        if (!helper_qualification) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper returned invalid v2 provenance for ",
                cache_file_stem(key), ": ", helper_qualification.error()));
        }
        if (!std::filesystem::exists(tmp_path, ec) || ec
            || std::filesystem::file_size(tmp_path, ec) == 0 || ec) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper produced no archive for ",
                cache_file_stem(key), ": ", *result));
        }
        auto archive_digest =
            sealed_shader_cache_detail::stream_stable_regular_file_digest(
            tmp_path,
            "qualified Metal binary archive",
            sealed_shader_cache_detail::maximum_seed_artifact_bytes);
        if (!archive_digest) {
            std::filesystem::remove(tmp_path, ec);
            return std::unexpected(archive_digest.error());
        }
        const MetalArchiveQualificationReceipt receipt{
            .wire_version =
                sealed_shader_cache_detail::archive_qualification_version,
            .shader_key = key,
            .metallib_size = metallib_size,
            .metallib_sha256 = metallib_sha256,
            .entry_name = std::string(entry_name),
            .max_total_threads_per_threadgroup = max_threads,
            .threadgroup_size_is_multiple = false,
            .negative_error = std::move(*negative_error),
            .archive_size = archive_digest->size,
            .archive_sha256 = archive_digest->sha256,
            .helper_protocol_version =
                helper_qualification->protocol_version,
            .helper_build_identity =
                std::move(helper_qualification->build_identity),
            .helper_source = qualified_helper.source,
            .helper_executable_size = qualified_helper.executable_size,
            .helper_executable_sha256 =
                qualified_helper.executable_sha256,
            .helper_metal_registry_id =
                helper_qualification->metal_registry_id,
        };
        auto receipt_bytes = sealed_shader_cache_detail::
            serialize_metal_archive_qualification_receipt(receipt);
        if (!receipt_bytes) {
            std::filesystem::remove(tmp_path, ec);
            return std::unexpected(receipt_bytes.error());
        }
        if (auto written = durable_write_file(
                qualification_tmp, *receipt_bytes);
            !written) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return written;
        }
        if (!sealed_shader_cache_detail::same_regular_file_identity(
                tmp_path, archive_digest->identity)) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return std::unexpected(
                "qualified Metal binary archive changed before publication");
        }
        if (auto renamed = durable_rename_existing(tmp_path, final_path); !renamed) {
            std::filesystem::remove(tmp_path, ec);
            std::filesystem::remove(qualification_tmp, ec);
            return renamed;
        }
        if (auto renamed = durable_rename_existing(
                qualification_tmp, qualification_path);
            !renamed) {
            std::filesystem::remove(qualification_tmp, ec);
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
make_moltenvk_metallib_module_code(std::span<const std::byte> metallib_bytes)
{
    if (metallib_bytes.empty()) {
        return std::unexpected("metallib empty");
    }
    if (metallib_bytes.size()
        > (std::numeric_limits<std::size_t>::max)()
            - sizeof(MVKMSLSPIRVHeader)) {
        return std::unexpected("metallib module size overflow");
    }

    MoltenVkShaderModuleCode code{};
    code.header_size_bytes = sizeof(MVKMSLSPIRVHeader);
    code.code_size_bytes = code.header_size_bytes + metallib_bytes.size();
    const std::size_t word_count =
        (code.code_size_bytes + sizeof(std::uint32_t) - 1u)
        / sizeof(std::uint32_t);
    const std::uint64_t exact_request = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(word_count), sizeof(std::uint32_t));
    const std::uint64_t peak_request = bioseq::parallelism::saturating_mul(
        exact_request,
        bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    if (exact_request == (std::numeric_limits<std::uint64_t>::max)()
        || peak_request == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected("metallib module admission size saturated");
    }
    bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(peak_request, &admission_failure);
    if (!permit) {
        return std::unexpected(shader_cache_message(
            "metallib module admission denied: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(
                admission_failure.reason),
            " requested=", admission_failure.requested_bytes));
    }
    code.permit = std::move(*permit);
    code.admission_count = 1u;
    code.peak_admitted_bytes = peak_request;
    code.words.resize(word_count);
    const std::uint64_t retained = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(code.words.capacity()),
        sizeof(std::uint32_t));
    if (retained == (std::numeric_limits<std::uint64_t>::max)()
        || retained > code.permit.bytes()) {
        code.admission_saturated = true;
        return std::unexpected(
            "metallib module allocation exceeded admitted peak");
    }
    code.permit.shrink_to(retained);
    code.retained_capacity_bytes = retained;

    auto* bytes = reinterpret_cast<std::byte*>(code.words.data());
    const MVKMSLSPIRVHeader header =
        static_cast<MVKMSLSPIRVHeader>(kMVKMagicNumberMSLCompiledCode);
    std::memcpy(bytes, &header, sizeof(header));
    std::memcpy(
        bytes + code.header_size_bytes,
        metallib_bytes.data(),
        metallib_bytes.size());
    return std::move(code);
}

[[nodiscard]] inline std::expected<MoltenVkShaderModuleCode, std::string>
make_moltenvk_metallib_module_code(const std::filesystem::path& metallib_path)
{
    auto bytes = sealed_shader_cache_detail::read_regular_file(
        metallib_path, "metallib");
    if (!bytes) return std::unexpected(bytes.error());
    return make_moltenvk_metallib_module_code(*bytes);
}

[[nodiscard]] inline std::expected<void, std::string> compile_variant_to_cache(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key,
    MslVersion msl_version,
    bool force_semantic_rebuild = false)
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
    const std::filesystem::path stem = cache_dir / cache_file_stem(key);
    const std::filesystem::path metal_path = stem.string() + ".tmp.metal";
    const std::filesystem::path air_path = stem.string() + ".air.tmp";
    const std::filesystem::path metallib_tmp = stem.string() + ".metallib.tmp";
    const std::filesystem::path metallib_final = cache_metallib_path(cache_dir, key);
    if (force_semantic_rebuild) {
        std::vector<std::filesystem::path> stale_paths{
            cache_meta_path(cache_dir, key),
            metallib_final,
            metal_path,
            air_path,
            metallib_tmp,
        };
        for (const std::filesystem::path& archive :
             cache_binary_archive_paths(cache_dir, key)) {
            stale_paths.push_back(archive);
            stale_paths.emplace_back(archive.string() + ".tmp");
            const std::filesystem::path qualification =
                cache_binary_archive_qualification_path(archive);
            stale_paths.push_back(qualification);
            stale_paths.emplace_back(qualification.string() + ".tmp");
        }
        for (const std::filesystem::path& stale : stale_paths) {
            ec.clear();
            std::filesystem::remove(stale, ec);
            if (ec) {
                return std::unexpected(shader_cache_message(
                    "remove unsealed shader artifact before semantic rebuild ",
                    stale.string(), " failed: ", ec.message()));
            }
        }
    }
    if (!force_semantic_rebuild && cache_entry_complete(cache_dir, key)) {
        return {};
    }
    if (!force_semantic_rebuild && cache_metallib_entry_complete(cache_dir, key)) {
        std::string existing_entry;
        auto existing_meta = read_meta(cache_meta_path(cache_dir, key), &existing_entry);
        if (!existing_meta) {
            return std::unexpected(existing_meta.error());
        }
        return ensure_binary_archive(
            cache_dir, key, *existing_meta, existing_entry, true);
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
    if (auto archive_written = ensure_binary_archive(
            cache_dir, key, meta, entry, true);
        !archive_written) {
        return archive_written;
    }
    std::filesystem::remove(metal_path, ec);
    std::filesystem::remove(air_path, ec);
    return {};
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

// Unsealed AOT construction adopts no prior semantic artifact. Metadata and
// metallib bytes are regenerated from the current in-process SPIR-V/layout/MSL
// pipeline, and every binary archive is then rebuilt and passes the reopen +
// FailOnBinaryArchiveMiss controls. A fully validated existing manifest is the
// only supervisor fast path.
[[nodiscard]] inline std::expected<void, std::string>
ensure_compiled_for_sealed_seed(
    const std::filesystem::path& cache_dir,
    const ShaderVariantKey& key,
    MslVersion version)
{
    auto compiled = compile_variant_to_cache(
        cache_dir, key, version, true);
    if (!compiled) return compiled;
    if (auto status = cache_entry_status(cache_dir, key); !status) {
        return std::unexpected(shader_cache_message(
            "sealed-seed compile produced incomplete entry for ",
            cache_file_stem(key), ": ", status.error()));
    }
    return {};
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

    ShaderModuleLoad result{
        .module = VK_NULL_HANDLE,
        .entry_name = std::move(entry),
        .wg_x = meta->wg_x,
        .wg_y = meta->wg_y,
        .wg_z = meta->wg_z,
    };
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = code->code_size_bytes;
    module_info.pCode = code->words.data();
    const VkResult vk_result =
        create_shader_module(device, &module_info, nullptr, &result.module);
    if (vk_result != VK_SUCCESS) {
        return std::unexpected(shader_cache_message(
            "vkCreateShaderModule(metallib): VkResult ",
            static_cast<int>(vk_result)));
    }
    return std::expected<ShaderModuleLoad, std::string>{
        std::in_place, std::move(result)};
}

[[nodiscard]] inline std::expected<ShaderModuleLoad, std::string>
create_shader_module_from_sealed_key_image(
    VkDevice device,
    const SealedShaderCacheKeyImage& image,
    PFN_vkCreateShaderModule create_shader_module)
{
    if (create_shader_module == nullptr) {
        return std::unexpected("vkCreateShaderModule function pointer is null");
    }
    if (image.entry_name.empty() || image.metallib_bytes.empty()
        || !(image.meta.key == image.key)) {
        return std::unexpected("sealed shader cache key image is incomplete");
    }
    auto code = make_moltenvk_metallib_module_code(image.metallib_bytes);
    if (!code) return std::unexpected(code.error());

    ShaderModuleLoad result{
        .module = VK_NULL_HANDLE,
        .entry_name = image.entry_name,
        .wg_x = image.meta.wg_x,
        .wg_y = image.meta.wg_y,
        .wg_z = image.meta.wg_z,
    };
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = code->code_size_bytes;
    module_info.pCode = code->words.data();
    const VkResult vk_result =
        create_shader_module(device, &module_info, nullptr, &result.module);
    if (vk_result != VK_SUCCESS) {
        return std::unexpected(shader_cache_message(
            "vkCreateShaderModule(sealed metallib): VkResult ",
            static_cast<int>(vk_result)));
    }
    return std::expected<ShaderModuleLoad, std::string>{
        std::in_place, std::move(result)};
}

[[nodiscard]] inline std::expected<ShaderModuleLoad, std::string>
create_shader_module_from_sealed_key_view(
    VkDevice device,
    const SealedShaderCacheKeyView& view,
    PFN_vkCreateShaderModule create_shader_module)
{
    if (create_shader_module == nullptr) {
        return std::unexpected("vkCreateShaderModule function pointer is null");
    }
    if (view.entry_name.empty() || view.metallib_bytes.empty()
        || !(view.meta.key == view.key)) {
        return std::unexpected("sealed shader cache key view is incomplete");
    }
    auto code = make_moltenvk_metallib_module_code(view.metallib_bytes);
    if (!code) return std::unexpected(code.error());

    ShaderModuleLoad result{
        .module = VK_NULL_HANDLE,
        .entry_name = view.entry_name,
        .wg_x = view.meta.wg_x,
        .wg_y = view.meta.wg_y,
        .wg_z = view.meta.wg_z,
    };
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = code->code_size_bytes;
    module_info.pCode = code->words.data();
    const VkResult vk_result =
        create_shader_module(device, &module_info, nullptr, &result.module);
    if (vk_result != VK_SUCCESS) {
        return std::unexpected(shader_cache_message(
            "vkCreateShaderModule(sealed seed metallib): VkResult ",
            static_cast<int>(vk_result)));
    }
    return std::expected<ShaderModuleLoad, std::string>{
        std::in_place, std::move(result)};
}

[[nodiscard]] inline std::expected<ShaderModuleLoad, std::string>
load_sealed_cached_module(
    VkDevice device,
    const SealedShaderCacheSeedImage& seed,
    const ShaderVariantKey& key,
    PFN_vkCreateShaderModule create_shader_module)
{
    auto view = sealed_shader_cache_key_view(seed, key);
    if (!view) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache process image rejected ",
            cache_file_stem(key), ": ", view.error()));
    }
    return create_shader_module_from_sealed_key_view(
        device, *view, create_shader_module);
}

[[nodiscard]] inline std::expected<ShaderModuleLoad, std::string>
load_sealed_cached_module(
    VkDevice device,
    const std::filesystem::path& cache_dir,
    std::string_view cache_digest,
    std::span<const ShaderVariantKey> exact_aot_keys,
    const ShaderVariantKey& key,
    PFN_vkCreateShaderModule create_shader_module)
{
    auto image = load_sealed_shader_cache_key_image(
        cache_dir, cache_digest, exact_aot_keys, key);
    if (!image) {
        return std::unexpected(shader_cache_message(
            "sealed shader cache provider rejected ", cache_file_stem(key),
            ": ", image.error()));
    }
    return create_shader_module_from_sealed_key_image(
        device, *image, create_shader_module);
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

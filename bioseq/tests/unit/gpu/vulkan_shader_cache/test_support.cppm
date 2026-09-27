/**
 * @file test_support.cppm
 * @brief Creates shader-cache test files and captures Vulkan module callbacks.
 */
module;

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.unit.gpu.vulkan_shader_cache_support;

import std;
import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.prefilter.types;

namespace gv = bioseq::gpu::vulkan;
namespace pf = bioseq::prefilter;

export namespace bioseq::tests::unit::gpu::vulkan_shader_cache_test_support {

bool shader_module_create_called = false;
bool shader_module_payload_matches = false;
std::vector<std::byte> expected_shader_module_metallib;
std::filesystem::path seal_identity_mutation_path;
bool seal_identity_mutation_succeeded = false;

/** Flips the first byte of the staged file without changing its size. */
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

/** Records a shader-module creation call and returns success. */
VKAPI_ATTR VkResult VKAPI_CALL record_shader_module_create(
    VkDevice,
    const VkShaderModuleCreateInfo*,
    const VkAllocationCallbacks*,
    VkShaderModule*)
{
    shader_module_create_called = true;
    return VK_SUCCESS;
}

/** Records whether Vulkan received the expected metallib bytes after its module header. */
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

/** @brief Counts compile requests without publishing cache artifacts. */
struct no_publish_compile_service {
    int calls = 0;

    [[nodiscard]] std::expected<void, std::string> ensure_variant_compiled(
        gv::shader_variant_key)
    {
        ++calls;
        return {};
    }
};

/** Creates a temporary cache directory and removes it on scope exit. */
struct temporary_directory {
    std::filesystem::path path;

    temporary_directory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
             / ("bioseq_shader_cache_test_" + std::to_string(::getpid())
                + "_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~temporary_directory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

/** @brief Restores one process environment variable when the test scope ends. */
struct scoped_environment_variable {
    std::string name;
    std::optional<std::string> previous;

    scoped_environment_variable(std::string variable, std::string_view value)
        : name(std::move(variable))
    {
        if (const char* existing = std::getenv(name.c_str()); existing != nullptr) {
            previous = existing;
        }
        REQUIRE(::setenv(name.c_str(), std::string(value).c_str(), 1) == 0);
    }

    ~scoped_environment_variable()
    {
        if (previous) {
            (void)::setenv(name.c_str(), previous->c_str(), 1);
        } else {
            (void)::unsetenv(name.c_str());
        }
    }
};

/** Clears the process archive-helper cache before and after the test. */
struct scoped_archive_helper_store_reset {
    scoped_archive_helper_store_reset() {
        REQUIRE(gv::qualified_metal_archive_helper_process_store_instance()
                    .reset_for_test());
    }
    ~scoped_archive_helper_store_reset() {
        (void)gv::qualified_metal_archive_helper_process_store_instance()
            .reset_for_test();
    }
};

/** Returns whether keys contains needle. */
[[nodiscard]] bool contains_key(
    const std::vector<gv::shader_variant_key>& keys,
    gv::shader_variant_key needle)
{
    return std::find(keys.begin(), keys.end(), needle) != keys.end();
}

/** Returns whether digest is exactly 16 lowercase hexadecimal characters. */
[[nodiscard]] bool is_hex_digest(std::string_view digest)
{
    return digest.size() == 16 && std::all_of(digest.begin(), digest.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

/** Copies the string bytes into an owned byte vector. */
[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text)
{
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

/** Reads the entire file into a byte vector, failing the test on a read error. */
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

/**
 * Writes a test validation record tying an archive to its shader and metallib bytes.
 * The record includes the entry name, thread limit and helper identity.
 */
void write_fake_archive_qualification(
    const std::filesystem::path& archive,
    std::span<const std::byte> archive_bytes,
    const gv::shader_variant_key& key,
    std::span<const std::byte> metallib_bytes,
    std::string_view entry_name,
    std::uint64_t max_threads = 256u)
{
#if defined(__APPLE__)
    const auto helper =
        gv::qualified_metal_archive_helper_process_store_instance().load_once();
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
    const gv::metal_archive_qualification_receipt qualification{
        .wire_revision = gv::sealed_shader_cache_detail::
            k_archive_qualification_wire_revision,
        .shader_key = key,
        .metallib_size = static_cast<std::uint64_t>(metallib_bytes.size()),
        .metallib_sha256 =
            gv::sealed_shader_cache_detail::sha256(metallib_bytes),
        .entry_name = std::string(entry_name),
        .max_total_threads_per_threadgroup = max_threads,
        .threadgroup_size_is_multiple = false,
        .negative_error = gv::metal_archive_negative_error_fingerprint{
            .domain = "MTLLibraryErrorDomain",
            .code = 6,
        },
        .archive_size = static_cast<std::uint64_t>(archive_bytes.size()),
        .archive_sha256 =
            gv::sealed_shader_cache_detail::sha256(archive_bytes),
        .helper_wire_revision = 2u,
        .helper_build_identity = "bioseq_gpu_archive_builder",
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

/** Builds fake MoltenVK command-library files with matching hashes and validation metadata. */
[[nodiscard]] gv::moltenvk_command_library_bundle
fake_moltenvk_command_library_bundle()
{
#if defined(__APPLE__)
    const auto helper =
        gv::qualified_metal_archive_helper_process_store_instance().load_once();
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
    std::string source =
        "kernel void cmdCopyBufferBytes() {}\n"
        "kernel void vtxCmdBlitImage() {}\n";
    source.append(256u, ' ');
    gv::moltenvk_command_library_bundle bundle{};
    bundle.source = bytes_of(source);
    bundle.metallib = bytes_of("qualified-command-metallib-byte-image");
    gv::moltenvk_command_compile_options options{
        .language_version = (3u << 16u) | 2u,
        .fast_math_enabled = 1u,
        .math_mode_available = 1u,
        .math_mode = 2u,
        .math_fp_functions_available = 1u,
        .math_fp_functions = 0u,
        .optimization_level_available = 1u,
        .optimization_level = 0u,
        .preserve_invariance = 0u,
        .library_type_available = 1u,
        .library_type = 0u,
        .compile_symbol_visibility_available = 1u,
        .compile_symbol_visibility = 0u,
        .allow_undefined_available = 1u,
        .allow_undefined = 0u,
        .max_total_threads_available = 1u,
        .max_total_threads = 0u,
        .enable_logging_available = 1u,
        .enable_logging = 0u,
        .preprocessor_macro_count = 0u,
        .linked_library_count = 0u,
        .install_name_bytes = 0u,
    };
    const auto loader = bytes_of("qualified-vulkan-loader-image");
    const auto moltenvk = bytes_of("qualified-moltenvk-image");
    bundle.qualification = gv::moltenvk_command_library_qualification_receipt{
        .wire_revision = gv::sealed_shader_cache_detail::k_command_library_wire_revision,
        .capture_count = 1u,
        .source_size = static_cast<std::uint64_t>(bundle.source.size()),
        .source_sha256 = gv::sealed_shader_cache_detail::sha256(bundle.source),
        .compile_options = options,
        .source_options_sha256 =
            gv::moltenvk_command_source_options_fingerprint(bundle.source, options),
        .metallib_size = static_cast<std::uint64_t>(bundle.metallib.size()),
        .metallib_sha256 = gv::sealed_shader_cache_detail::sha256(bundle.metallib),
        .helper_wire_revision = 2u,
        .helper_build_identity = "bioseq_gpu_archive_builder",
        .helper_source = helper_source,
        .helper_executable_size = helper_size,
        .helper_executable_sha256 = helper_sha256,
        .helper_metal_registry_id = 0x1234u,
        .vulkan_device = gv::moltenvk_command_vulkan_identity{
            .device_uuid = {0x42u},
            .api_version = 0x00403000u,
            .driver_version = 0x01020304u,
            .vendor_id = 0x0000106bu,
            .device_id = 0x00000001u,
            .pipeline_cache_uuid = {0x42u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
                                    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x24u},
        },
        .loader_image_size = static_cast<std::uint64_t>(loader.size()),
        .loader_image_sha256 = gv::sealed_shader_cache_detail::sha256(loader),
        .moltenvk_image_size = static_cast<std::uint64_t>(moltenvk.size()),
        .moltenvk_image_sha256 = gv::sealed_shader_cache_detail::sha256(moltenvk),
    };
    REQUIRE(bundle.is_accepted());
    return bundle;
}

/** Writes fake shader libraries, metadata and archive records for the requested variants. */
void write_fake_sealed_shader_artifacts(
    const std::filesystem::path& cache_dir,
    std::span<const gv::shader_variant_key> keys)
{
    constexpr std::string_view entry = "main0";
    const auto command_bundle = fake_moltenvk_command_library_bundle();
    const auto command_wire = gv::sealed_shader_cache_detail::
        serialize_moltenvk_command_library_bundle(command_bundle);
    REQUIRE(command_wire);
    REQUIRE(gv::durable_write_file(
        gv::cache_moltenvk_command_library_path(cache_dir), *command_wire));
    for (const gv::shader_variant_key& key : keys) {
        const gv::shader_workgroup_size workgroup =
            gv::expected_shader_workgroup_size(key);
        gv::shader_cache_metadata meta{};
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

/**
 * Counts the requested SPIR-V opcode.
 * Returns zero for a short header, a wrong magic word or invalid instruction lengths.
 */
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

} // namespace bioseq::tests::unit::gpu::vulkan_shader_cache_test_support

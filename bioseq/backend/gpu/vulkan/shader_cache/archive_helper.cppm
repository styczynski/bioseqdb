/**
 * @file archive_helper.cppm
 * @brief Builds Metal archives and records which shader, helper, and device produced them.
 */
module;

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#if defined(__APPLE__) && !defined(BIOSEQ_RECEIPT_OWNED_VULKAN_LOADER_PATH)
#error "Apple command capture requires one receipt-owned Vulkan loader path"
#endif

export module bioseq.gpu.vulkan.shader_cache:archive_helper;

import :paths_metadata;
import :subprocess;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import std;

export namespace bioseq::gpu::vulkan {

/// @brief NSError domain and code returned when an empty archive fails to create a pipeline.
struct metal_archive_negative_error_fingerprint {
    std::string domain;
    std::int64_t code = 0;

    /// @brief Checks that the error domain is nonempty, bounded, and contains no NUL bytes.
    [[nodiscard]] bool is_accepted() const noexcept {
        return !domain.empty() && domain.size() <= 1024u && domain.find('\0') == std::string::npos;
    }
    /// @brief Compares the NSError domain and code.
    [[nodiscard]] bool
    operator==(const metal_archive_negative_error_fingerprint&) const noexcept = default;
};

/// @brief Records a checked Metal archive and the inputs used to build it.
/// Includes the shader library, pipeline settings, helper executable, Metal
/// device, and error expected when the same pipeline uses an empty archive.
struct metal_archive_qualification_receipt {
    std::uint32_t wire_revision = 0u;
    shader_variant_key shader_key{};
    std::uint64_t metallib_size = 0u;
    std::array<std::uint8_t, 32> metallib_sha256{};
    std::string entry_name;
    std::uint64_t max_total_threads_per_threadgroup = 0u;
    bool threadgroup_size_is_multiple = false;
    metal_archive_negative_error_fingerprint negative_error;
    std::uint64_t archive_size = 0u;
    std::array<std::uint8_t, 32> archive_sha256{};
    std::uint32_t helper_wire_revision = 0u;
    std::string helper_build_identity;
    std::string helper_source;
    std::uint64_t helper_executable_size = 0u;
    std::array<std::uint8_t, 32> helper_executable_sha256{};
    std::uint64_t helper_metal_registry_id = 0u;

    /// @brief Checks the archive size/hash, thread limit, error record, and format version.
    /// The remaining build identities are checked by `is_accepted`.
    [[nodiscard]] bool is_diagnostic_valid() const noexcept {
        return wire_revision == 2u && max_total_threads_per_threadgroup > 0u &&
               max_total_threads_per_threadgroup <= 1024u && negative_error.is_accepted() &&
               archive_size != 0u &&
               std::any_of(archive_sha256.begin(), archive_sha256.end(), [](std::uint8_t byte) {
                   return byte != 0u;
               });
    }
    /// @brief Checks that all required build identities and archive fields are present and valid.
    /// This checks the record itself. File hashes are verified when loading the seed.
    [[nodiscard]] bool is_accepted() const noexcept {
        // A zero digest marks a missing hash.
        const auto nonzero_digest = [](const auto& digest) {
            return std::any_of(
                digest.begin(), digest.end(), [](std::uint8_t byte) { return byte != 0u; });
        };
        return is_diagnostic_valid() && is_shader_variant_key_valid(shader_key) &&
               metallib_size != 0u && nonzero_digest(metallib_sha256) && !entry_name.empty() &&
               entry_name.size() <= 1024u && entry_name.find('\0') == std::string::npos &&
               helper_wire_revision == 2u && !helper_build_identity.empty() &&
               helper_build_identity.size() <= 1024u &&
               helper_build_identity.find('\0') == std::string::npos && !helper_source.empty() &&
               helper_source.size() <= 64u && helper_source.find('\0') == std::string::npos &&
               helper_executable_size != 0u && nonzero_digest(helper_executable_sha256) &&
               helper_metal_registry_id != 0u;
    }
    /// @brief Requires a valid record naming the installed bioseq_gpu_archive_builder helper.
    [[nodiscard]] bool is_production_accepted() const noexcept {
        return is_accepted() && helper_source == "installed-sibling" &&
               helper_build_identity == "bioseq_gpu_archive_builder";
    }
    /// @brief Compares every serialized qualification field.
    [[nodiscard]] bool
    operator==(const metal_archive_qualification_receipt&) const noexcept = default;
};

/// @brief Metal compiler options captured while MoltenVK builds its command library.
struct moltenvk_command_compile_options {
    std::uint64_t language_version = 0u;
    std::uint64_t fast_math_enabled = 0u;
    std::uint64_t math_mode_available = 0u;
    std::uint64_t math_mode = 0u;
    std::uint64_t math_fp_functions_available = 0u;
    std::uint64_t math_fp_functions = 0u;
    std::uint64_t optimization_level_available = 0u;
    std::uint64_t optimization_level = 0u;
    std::uint64_t preserve_invariance = 0u;
    std::uint64_t library_type_available = 0u;
    std::uint64_t library_type = 0u;
    std::uint64_t compile_symbol_visibility_available = 0u;
    std::uint64_t compile_symbol_visibility = 0u;
    std::uint64_t allow_undefined_available = 0u;
    std::uint64_t allow_undefined = 0u;
    std::uint64_t max_total_threads_available = 0u;
    std::uint64_t max_total_threads = 0u;
    std::uint64_t enable_logging_available = 0u;
    std::uint64_t enable_logging = 0u;
    std::uint64_t preprocessor_macro_count = 0u;
    std::uint64_t linked_library_count = 0u;
    std::uint64_t install_name_bytes = 0u;

    /// @brief Checks that the captured options can be passed to the system Metal compiler.
    /// Rejects unsupported features such as macros, linked libraries, and custom install names.
    [[nodiscard]] bool is_accepted() const noexcept {
        const auto boolean = [](std::uint64_t value) { return value <= 1u; };
        return language_version != 0u && boolean(fast_math_enabled) &&
               boolean(math_mode_available) && math_mode <= 2u &&
               boolean(math_fp_functions_available) && math_fp_functions <= 1u &&
               math_mode_available == math_fp_functions_available &&
               boolean(optimization_level_available) && optimization_level <= 1u &&
               boolean(preserve_invariance) && boolean(library_type_available) &&
               library_type == 0u && boolean(compile_symbol_visibility_available) &&
               compile_symbol_visibility == 0u && boolean(allow_undefined_available) &&
               allow_undefined == 0u && boolean(max_total_threads_available) &&
               max_total_threads == 0u && boolean(enable_logging_available) &&
               enable_logging == 0u && preprocessor_macro_count == 0u &&
               linked_library_count == 0u && install_name_bytes == 0u;
    }

    [[nodiscard]] bool operator==(const moltenvk_command_compile_options&) const noexcept = default;
};

/// @brief Vulkan device and driver IDs recorded during MoltenVK command capture.
struct moltenvk_command_vulkan_identity {
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};

    [[nodiscard]] bool is_accepted() const noexcept {
        const auto nonzero = [](const auto& bytes) {
            return std::any_of(
                bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte != 0u; });
        };
        return api_version != 0u && driver_version != 0u && nonzero(device_uuid) &&
               nonzero(pipeline_cache_uuid);
    }

    [[nodiscard]] bool operator==(const moltenvk_command_vulkan_identity&) const noexcept = default;
};

/// @brief Hashes the command source and compiler options so runtime can recognize the same request.
[[nodiscard]] inline std::array<std::uint8_t, 32>
moltenvk_command_source_options_fingerprint(
    std::span<const std::byte> source,
    const moltenvk_command_compile_options& options) noexcept {
    sealed_shader_cache_detail::sha256_hasher hasher;
    hasher.update("bioseq-moltenvk-command-source-options-v1");
    hasher.update_little_endian(source.size());
    hasher.update(source);
    const std::array values{
        options.language_version,
        options.fast_math_enabled,
        options.math_mode_available,
        options.math_mode,
        options.math_fp_functions_available,
        options.math_fp_functions,
        options.optimization_level_available,
        options.optimization_level,
        options.preserve_invariance,
        options.library_type_available,
        options.library_type,
        options.compile_symbol_visibility_available,
        options.compile_symbol_visibility,
        options.allow_undefined_available,
        options.allow_undefined,
        options.max_total_threads_available,
        options.max_total_threads,
        options.enable_logging_available,
        options.enable_logging,
        options.preprocessor_macro_count,
        options.linked_library_count,
        options.install_name_bytes,
    };
    for (const std::uint64_t value : values)
        hasher.update_little_endian(value);
    return hasher.finalize();
}

/// @brief Records how a precompiled MoltenVK command library was produced.
/// Binds the source and options to the metallib, helper executable, Vulkan
/// device, loader, and MoltenVK library used during capture.
struct moltenvk_command_library_qualification_receipt {
    std::uint32_t wire_revision = 0u;
    std::uint64_t capture_count = 0u;
    std::uint64_t source_size = 0u;
    std::array<std::uint8_t, 32> source_sha256{};
    moltenvk_command_compile_options compile_options{};
    std::array<std::uint8_t, 32> source_options_sha256{};
    std::uint64_t metallib_size = 0u;
    std::array<std::uint8_t, 32> metallib_sha256{};
    std::uint32_t helper_wire_revision = 0u;
    std::string helper_build_identity;
    std::string helper_source;
    std::uint64_t helper_executable_size = 0u;
    std::array<std::uint8_t, 32> helper_executable_sha256{};
    std::uint64_t helper_metal_registry_id = 0u;
    moltenvk_command_vulkan_identity vulkan_device{};
    std::uint64_t loader_image_size = 0u;
    std::array<std::uint8_t, 32> loader_image_sha256{};
    std::uint64_t moltenvk_image_size = 0u;
    std::array<std::uint8_t, 32> moltenvk_image_sha256{};

    [[nodiscard]] bool is_accepted() const noexcept {
        const auto nonzero = [](const auto& digest) {
            return std::any_of(
                digest.begin(), digest.end(), [](std::uint8_t byte) { return byte != 0u; });
        };
        return wire_revision == 1u && capture_count == 1u && source_size != 0u &&
               nonzero(source_sha256) && compile_options.is_accepted() &&
               nonzero(source_options_sha256) && metallib_size != 0u &&
               nonzero(metallib_sha256) && helper_wire_revision == 2u &&
               !helper_build_identity.empty() && helper_build_identity.size() <= 1024u &&
               helper_build_identity.find('\0') == std::string::npos && !helper_source.empty() &&
               helper_source.size() <= 64u && helper_source.find('\0') == std::string::npos &&
               helper_executable_size != 0u && nonzero(helper_executable_sha256) &&
               helper_metal_registry_id != 0u && vulkan_device.is_accepted() &&
               loader_image_size != 0u && nonzero(loader_image_sha256) &&
               moltenvk_image_size != 0u && nonzero(moltenvk_image_sha256);
    }

    [[nodiscard]] bool is_production_accepted() const noexcept {
        return is_accepted() && helper_source == "installed-sibling" &&
               helper_build_identity == "bioseq_gpu_archive_builder";
    }

    [[nodiscard]] bool operator==(
        const moltenvk_command_library_qualification_receipt&) const noexcept = default;
};

/// @brief Owns MoltenVK command source, its compiled library, and the record connecting them.
struct moltenvk_command_library_bundle {
    moltenvk_command_library_qualification_receipt qualification{};
    std::vector<std::byte> source;
    std::vector<std::byte> metallib;

    [[nodiscard]] bool is_accepted() const noexcept {
        if (!qualification.is_accepted() || source.size() != qualification.source_size ||
            metallib.size() != qualification.metallib_size || source.empty() || metallib.empty() ||
            sealed_shader_cache_detail::sha256(source) != qualification.source_sha256 ||
            sealed_shader_cache_detail::sha256(metallib) != qualification.metallib_sha256 ||
            moltenvk_command_source_options_fingerprint(source, qualification.compile_options) !=
                qualification.source_options_sha256) {
            return false;
        }
        const std::string_view text{reinterpret_cast<const char*>(source.data()), source.size()};
        return text.find('\0') == std::string_view::npos &&
               text.find("cmdCopyBufferBytes") != std::string_view::npos &&
               text.find("vtxCmdBlitImage") != std::string_view::npos;
    }

    [[nodiscard]] bool operator==(const moltenvk_command_library_bundle&) const noexcept = default;
};

/// @brief Checks that each required Metal archive and its receipt file are nonempty.
/// On non-Apple systems this succeeds without checking archives.
/// @return Success or the first missing-file, size, or filesystem error.
[[nodiscard]] inline std::expected<void, std::string>
cache_binary_archive_status(const std::filesystem::path& cache_dir, const shader_variant_key& key) {
#if !defined(__APPLE__)
    (void)cache_dir;
    (void)key;
    return {};
#else
    for (const std::filesystem::path& archive : cache_binary_archive_paths(cache_dir, key)) {
        if (auto present = require_nonempty_cache_file(archive, "Metal binary archive"); !present) {
            return present;
        }
        if (auto qualified =
                require_nonempty_cache_file(cache_binary_archive_qualification_path(archive),
                                            "Metal binary archive qualification receipt");
            !qualified) {
            return qualified;
        }
    }
    return {};
#endif
}

/// @brief Reports whether all required archive and receipt files are nonempty.
/// Returns true on non-Apple systems. File contents are checked when sealing or loading.
[[nodiscard]] inline bool cache_binary_archive_complete(const std::filesystem::path& cache_dir,
                                                        const shader_variant_key& key) {
    return cache_binary_archive_status(cache_dir, key).has_value();
}

/// @brief Checks shader metadata and the presence of the required cache files.
/// Archive and library contents are checked separately when sealing or loading.
/// @return Success or the first metadata or file error.
[[nodiscard]] inline std::expected<void, std::string>
cache_entry_status(const std::filesystem::path& cache_dir, const shader_variant_key& key) {
    if (auto metallib = cache_metallib_entry_status(cache_dir, key); !metallib) {
        return metallib;
    }
    return cache_binary_archive_status(cache_dir, key);
}

/// @brief Reports whether shader metadata is valid and the required cache files are nonempty.
[[nodiscard]] inline bool cache_entry_complete(const std::filesystem::path& cache_dir,
                                               const shader_variant_key& key) {
    return cache_entry_status(cache_dir, key).has_value();
}

namespace sealed_shader_cache_detail {

/// @brief File signature identifying a Metal archive receipt.
inline constexpr std::string_view k_archive_qualification_magic = "BAMISS01";
/// @brief Version of the serialized Metal archive receipt format.
inline constexpr std::uint32_t k_archive_qualification_wire_revision = 2u;
/// @brief Maximum accepted archive qualification record size.
inline constexpr std::size_t k_maximum_archive_qualification_bytes = 4096u;
/// @brief File signature identifying a MoltenVK command-library bundle.
inline constexpr std::string_view k_command_library_magic = "BMVKCMD1";
/// @brief Version of the serialized MoltenVK command-library bundle format.
inline constexpr std::uint32_t k_command_library_wire_revision = 1u;
/// @brief Maximum source size accepted in a MoltenVK command-library bundle.
inline constexpr std::uint64_t k_maximum_command_source_bytes = 64u << 20u;

/// @brief Encodes a valid Metal archive receipt and appends a checksum.
/// Returns bytes or an error if required fields are missing or the record is too large.
[[nodiscard]] inline std::expected<std::vector<std::byte>, std::string>
serialize_metal_archive_qualification_receipt(const metal_archive_qualification_receipt& receipt) {
    if (!receipt.is_accepted()) {
        return std::unexpected("Metal archive qualification receipt is incomplete");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(256u + receipt.entry_name.size() + receipt.negative_error.domain.size() +
                  receipt.helper_build_identity.size() + receipt.helper_source.size());
    append_text(bytes, k_archive_qualification_magic);
    append_u32(bytes, k_archive_qualification_wire_revision);
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
    append_u32(bytes, static_cast<std::uint32_t>(receipt.negative_error.domain.size()));
    append_u64(bytes, std::bit_cast<std::uint64_t>(receipt.negative_error.code));
    append_u64(bytes, receipt.archive_size);
    append_digest(bytes, receipt.archive_sha256);
    append_u32(bytes, receipt.helper_wire_revision);
    append_u32(bytes, static_cast<std::uint32_t>(receipt.helper_build_identity.size()));
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
    if (bytes.size() > k_maximum_archive_qualification_bytes) {
        return std::unexpected("Metal archive qualification receipt is too large");
    }
    return bytes;
}

/// @brief Reads a Metal archive receipt and checks its format, fields, and checksum.
/// Returns the parsed record or an error. The referenced files are checked separately.
[[nodiscard]] inline std::expected<metal_archive_qualification_receipt, std::string>
parse_metal_archive_qualification_receipt(std::span<const std::byte> bytes) {
    constexpr std::size_t k_minimum_fixed_bytes = 8u + 2u * 4u + 8u;
    if (bytes.size() < k_minimum_fixed_bytes ||
        bytes.size() > k_maximum_archive_qualification_bytes) {
        return std::unexpected("Metal archive qualification receipt size is invalid");
    }
    const std::size_t checksum_offset = bytes.size() - sizeof(std::uint64_t);
    manifest_reader reader(bytes);
    auto magic = reader.text(k_archive_qualification_magic.size());
    auto wire_revision = reader.u32();
    auto reserved = reader.u32();
    if (!magic || !wire_revision || !reserved || *magic != k_archive_qualification_magic ||
        *reserved != 0u) {
        return std::unexpected("Metal archive qualification receipt header is invalid");
    }
    if (*wire_revision != k_archive_qualification_wire_revision) {
        return std::unexpected("Metal archive qualification receipt contract mismatch");
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
    auto helper_wire_revision = reader.u32();
    auto helper_build_size = reader.u32();
    auto helper_source_size = reader.u32();
    auto reserved2 = reader.u32();
    auto helper_executable_size = reader.u64();
    auto helper_executable_sha = reader.digest();
    auto helper_registry_id = reader.u64();
    if (!key_kind || !key_tile || !key_local || !key_span || !metallib_size || !metallib_sha ||
        !entry_size || !max_threads || !multiple || !domain_size || !code_bits || !archive_size ||
        !archive_sha || !helper_wire_revision || !helper_build_size || !helper_source_size ||
        !reserved2 || !helper_executable_size || !helper_executable_sha || !helper_registry_id) {
        return std::unexpected("Metal archive qualification receipt is truncated");
    }
    if (*reserved2 != 0u || *multiple > 1u || *entry_size == 0u || *entry_size > 1024u ||
        *domain_size == 0u || *domain_size > 1024u || *helper_build_size == 0u ||
        *helper_build_size > 1024u || *helper_source_size == 0u || *helper_source_size > 64u) {
        return std::unexpected("Metal archive qualification receipt header is invalid");
    }
    auto entry_name = reader.text(*entry_size);
    auto domain = reader.text(*domain_size);
    auto helper_build = reader.text(*helper_build_size);
    auto helper_source = reader.text(*helper_source_size);
    auto stored_checksum = reader.u64();
    if (!entry_name || !domain || !helper_build || !helper_source || !stored_checksum ||
        !reader.at_end()) {
        return std::unexpected("Metal archive qualification receipt payload is invalid");
    }
    if (*stored_checksum != manifest_checksum(bytes.first(checksum_offset))) {
        return std::unexpected("Metal archive qualification receipt checksum mismatch");
    }
    metal_archive_qualification_receipt receipt{
        .wire_revision = k_archive_qualification_wire_revision,
        .shader_key =
            shader_variant_key{
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
        .negative_error =
            metal_archive_negative_error_fingerprint{
                .domain = std::move(*domain),
                .code = std::bit_cast<std::int64_t>(*code_bits),
            },
        .archive_size = *archive_size,
        .archive_sha256 = *archive_sha,
        .helper_wire_revision = *helper_wire_revision,
        .helper_build_identity = std::move(*helper_build),
        .helper_source = std::move(*helper_source),
        .helper_executable_size = *helper_executable_size,
        .helper_executable_sha256 = *helper_executable_sha,
        .helper_metal_registry_id = *helper_registry_id,
    };
    if (!receipt.is_accepted()) {
        return std::unexpected("Metal archive qualification receipt values are invalid");
    }
    return receipt;
}

/// @brief Writes command compiler options in the order used by the bundle format.
inline void append_moltenvk_command_compile_options(
    std::vector<std::byte>& bytes,
    const moltenvk_command_compile_options& options) {
    const std::array values{
        options.language_version,
        options.fast_math_enabled,
        options.math_mode_available,
        options.math_mode,
        options.math_fp_functions_available,
        options.math_fp_functions,
        options.optimization_level_available,
        options.optimization_level,
        options.preserve_invariance,
        options.library_type_available,
        options.library_type,
        options.compile_symbol_visibility_available,
        options.compile_symbol_visibility,
        options.allow_undefined_available,
        options.allow_undefined,
        options.max_total_threads_available,
        options.max_total_threads,
        options.enable_logging_available,
        options.enable_logging,
        options.preprocessor_macro_count,
        options.linked_library_count,
        options.install_name_bytes,
    };
    for (const std::uint64_t value : values)
        append_u64(bytes, value);
}

/// @brief Reads command compiler options and rejects unsupported settings.
[[nodiscard]] inline std::expected<moltenvk_command_compile_options, std::string>
read_moltenvk_command_compile_options(manifest_reader& reader) {
    std::array<std::uint64_t, 22u> values{};
    for (std::uint64_t& value : values) {
        auto field = reader.u64();
        if (!field)
            return std::unexpected("MoltenVK command compile options are truncated");
        value = *field;
    }
    moltenvk_command_compile_options options{
        .language_version = values[0],
        .fast_math_enabled = values[1],
        .math_mode_available = values[2],
        .math_mode = values[3],
        .math_fp_functions_available = values[4],
        .math_fp_functions = values[5],
        .optimization_level_available = values[6],
        .optimization_level = values[7],
        .preserve_invariance = values[8],
        .library_type_available = values[9],
        .library_type = values[10],
        .compile_symbol_visibility_available = values[11],
        .compile_symbol_visibility = values[12],
        .allow_undefined_available = values[13],
        .allow_undefined = values[14],
        .max_total_threads_available = values[15],
        .max_total_threads = values[16],
        .enable_logging_available = values[17],
        .enable_logging = values[18],
        .preprocessor_macro_count = values[19],
        .linked_library_count = values[20],
        .install_name_bytes = values[21],
    };
    if (!options.is_accepted())
        return std::unexpected("MoltenVK command compile options are unsupported");
    return options;
}

/// @brief Encodes command source, its compiled library, and their build record with a checksum.
/// Checks their hashes and size limits before producing the bytes.
[[nodiscard]] inline std::expected<std::vector<std::byte>, std::string>
serialize_moltenvk_command_library_bundle(const moltenvk_command_library_bundle& bundle) {
    if (!bundle.is_accepted())
        return std::unexpected("MoltenVK command-library bundle is incomplete");
    const auto& receipt = bundle.qualification;
    const std::uint64_t payload_size = bioseq::common::saturating_add(
        static_cast<std::uint64_t>(bundle.source.size()),
        static_cast<std::uint64_t>(bundle.metallib.size()));
    if (payload_size == (std::numeric_limits<std::uint64_t>::max)() ||
        bundle.source.size() > k_maximum_command_source_bytes ||
        payload_size > k_maximum_seed_artifact_bytes) {
        return std::unexpected("MoltenVK command-library bundle exceeds artifact bounds");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(static_cast<std::size_t>(payload_size) + 1024u);
    append_text(bytes, k_command_library_magic);
    append_u32(bytes, k_command_library_wire_revision);
    append_u32(bytes, 0u);
    append_u64(bytes, receipt.capture_count);
    append_u64(bytes, receipt.source_size);
    append_digest(bytes, receipt.source_sha256);
    append_u64(bytes, receipt.metallib_size);
    append_digest(bytes, receipt.metallib_sha256);
    append_digest(bytes, receipt.source_options_sha256);
    append_moltenvk_command_compile_options(bytes, receipt.compile_options);
    append_u32(bytes, receipt.helper_wire_revision);
    append_u32(bytes, static_cast<std::uint32_t>(receipt.helper_build_identity.size()));
    append_u32(bytes, static_cast<std::uint32_t>(receipt.helper_source.size()));
    append_u32(bytes, 0u);
    append_u64(bytes, receipt.helper_executable_size);
    append_digest(bytes, receipt.helper_executable_sha256);
    append_u64(bytes, receipt.helper_metal_registry_id);
    const auto append_fixed = [&bytes](const auto& value) {
        const auto view = std::as_bytes(std::span{value});
        bytes.insert(bytes.end(), view.begin(), view.end());
    };
    append_fixed(receipt.vulkan_device.device_uuid);
    append_u32(bytes, receipt.vulkan_device.api_version);
    append_u32(bytes, receipt.vulkan_device.driver_version);
    append_u32(bytes, receipt.vulkan_device.vendor_id);
    append_u32(bytes, receipt.vulkan_device.device_id);
    append_fixed(receipt.vulkan_device.pipeline_cache_uuid);
    append_u64(bytes, receipt.loader_image_size);
    append_digest(bytes, receipt.loader_image_sha256);
    append_u64(bytes, receipt.moltenvk_image_size);
    append_digest(bytes, receipt.moltenvk_image_sha256);
    append_text(bytes, receipt.helper_build_identity);
    append_text(bytes, receipt.helper_source);
    bytes.insert(bytes.end(), bundle.source.begin(), bundle.source.end());
    bytes.insert(bytes.end(), bundle.metallib.begin(), bundle.metallib.end());
    append_u64(bytes, manifest_checksum(bytes));
    if (bytes.size() > k_maximum_seed_artifact_bytes)
        return std::unexpected("MoltenVK command-library wire image is too large");
    return bytes;
}

/// @brief Reads a command-library bundle and verifies its sizes, checksum, and content hashes.
/// Also checks the compiler options and required build/device fields.
[[nodiscard]] inline std::expected<moltenvk_command_library_bundle, std::string>
parse_moltenvk_command_library_bundle(std::span<const std::byte> bytes) {
    if (bytes.size() < 512u || bytes.size() > k_maximum_seed_artifact_bytes)
        return std::unexpected("MoltenVK command-library bundle size is invalid");
    const std::size_t checksum_offset = bytes.size() - sizeof(std::uint64_t);
    manifest_reader reader(bytes);
    auto magic = reader.text(k_command_library_magic.size());
    auto wire_revision = reader.u32();
    auto reserved = reader.u32();
    auto capture_count = reader.u64();
    auto source_size = reader.u64();
    auto source_sha = reader.digest();
    auto metallib_size = reader.u64();
    auto metallib_sha = reader.digest();
    auto source_options_sha = reader.digest();
    if (!magic || !wire_revision || !reserved || !capture_count || !source_size || !source_sha ||
        !metallib_size || !metallib_sha || !source_options_sha ||
        *magic != k_command_library_magic || *wire_revision != k_command_library_wire_revision ||
        *reserved != 0u || *source_size == 0u || *source_size > k_maximum_command_source_bytes ||
        *metallib_size == 0u || *metallib_size > k_maximum_seed_artifact_bytes ||
        *source_size > (std::numeric_limits<std::size_t>::max)() ||
        *metallib_size > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected("MoltenVK command-library bundle header is invalid");
    }
    auto options = read_moltenvk_command_compile_options(reader);
    auto helper_wire_revision = reader.u32();
    auto helper_build_size = reader.u32();
    auto helper_source_size = reader.u32();
    auto reserved2 = reader.u32();
    auto helper_executable_size = reader.u64();
    auto helper_executable_sha = reader.digest();
    auto helper_registry_id = reader.u64();
    if (!options || !helper_wire_revision || !helper_build_size || !helper_source_size ||
        !reserved2 || !helper_executable_size || !helper_executable_sha || !helper_registry_id ||
        *reserved2 != 0u || *helper_build_size == 0u || *helper_build_size > 1024u ||
        *helper_source_size == 0u || *helper_source_size > 64u) {
        return std::unexpected("MoltenVK command-library helper provenance is invalid");
    }
    auto device_uuid = reader.take(VK_UUID_SIZE);
    auto api_version = reader.u32();
    auto driver_version = reader.u32();
    auto vendor_id = reader.u32();
    auto device_id = reader.u32();
    auto pipeline_uuid = reader.take(VK_UUID_SIZE);
    auto loader_size = reader.u64();
    auto loader_sha = reader.digest();
    auto moltenvk_size = reader.u64();
    auto moltenvk_sha = reader.digest();
    if (!device_uuid || !api_version || !driver_version || !vendor_id || !device_id ||
        !pipeline_uuid || !loader_size || !loader_sha || !moltenvk_size || !moltenvk_sha) {
        return std::unexpected("MoltenVK command-library device/loader provenance is truncated");
    }
    auto helper_build = reader.text(*helper_build_size);
    auto helper_source = reader.text(*helper_source_size);
    auto source = reader.take(static_cast<std::size_t>(*source_size));
    auto metallib = reader.take(static_cast<std::size_t>(*metallib_size));
    auto stored_checksum = reader.u64();
    if (!helper_build || !helper_source || !source || !metallib || !stored_checksum ||
        !reader.at_end()) {
        return std::unexpected("MoltenVK command-library bundle payload is invalid");
    }
    if (*stored_checksum != manifest_checksum(bytes.first(checksum_offset)))
        return std::unexpected("MoltenVK command-library bundle checksum mismatch");
    moltenvk_command_library_bundle bundle{};
    bundle.qualification.wire_revision = k_command_library_wire_revision;
    bundle.qualification.capture_count = *capture_count;
    bundle.qualification.source_size = *source_size;
    bundle.qualification.source_sha256 = *source_sha;
    bundle.qualification.compile_options = *options;
    bundle.qualification.source_options_sha256 = *source_options_sha;
    bundle.qualification.metallib_size = *metallib_size;
    bundle.qualification.metallib_sha256 = *metallib_sha;
    bundle.qualification.helper_wire_revision = *helper_wire_revision;
    bundle.qualification.helper_build_identity = std::move(*helper_build);
    bundle.qualification.helper_source = std::move(*helper_source);
    bundle.qualification.helper_executable_size = *helper_executable_size;
    bundle.qualification.helper_executable_sha256 = *helper_executable_sha;
    bundle.qualification.helper_metal_registry_id = *helper_registry_id;
    std::memcpy(bundle.qualification.vulkan_device.device_uuid.data(),
                device_uuid->data(),
                VK_UUID_SIZE);
    bundle.qualification.vulkan_device.api_version = *api_version;
    bundle.qualification.vulkan_device.driver_version = *driver_version;
    bundle.qualification.vulkan_device.vendor_id = *vendor_id;
    bundle.qualification.vulkan_device.device_id = *device_id;
    std::memcpy(bundle.qualification.vulkan_device.pipeline_cache_uuid.data(),
                pipeline_uuid->data(),
                VK_UUID_SIZE);
    bundle.qualification.loader_image_size = *loader_size;
    bundle.qualification.loader_image_sha256 = *loader_sha;
    bundle.qualification.moltenvk_image_size = *moltenvk_size;
    bundle.qualification.moltenvk_image_sha256 = *moltenvk_sha;
    bundle.source.assign(source->begin(), source->end());
    bundle.metallib.assign(metallib->begin(), metallib->end());
    if (!bundle.is_accepted())
        return std::unexpected("MoltenVK command-library bundle evidence is inconsistent");
    return bundle;
}

} // namespace sealed_shader_cache_detail

/// @brief Owns the checked archive-helper executable bytes and their memory reservation.
/// Later invocations copy these bytes into a private executable file.
struct qualified_metal_archive_helper {
    bioseq::parallelism::memory_permit permit;
    std::filesystem::path path;
    sealed_shader_cache_detail::regular_file_identity identity{};
    std::uint64_t executable_size = 0u;
    std::array<std::uint8_t, 32> executable_sha256{};
    std::vector<std::byte> executable_bytes;
    std::string source;
    std::uint64_t hash_passes = 0u;

    /// @brief Checks that the saved helper image has its identity fields and memory reservation.
    /// Does not read the executable path or recompute its hash.
    [[nodiscard]] bool is_accepted() const noexcept {
        return permit && permit.bytes() == owned_capacity_bytes() && !path.empty() &&
               executable_size != 0u && hash_passes == 1u &&
               executable_bytes.size() == executable_size &&
               (source == "installed-sibling" || source == "test-env-override") &&
               std::any_of(executable_sha256.begin(),
                           executable_sha256.end(),
                           [](std::uint8_t byte) { return byte != 0u; });
    }

    /// @brief Counts the helper object and the allocated capacity of its data.
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::common::saturating_add;
        std::uint64_t owned = sizeof(qualified_metal_archive_helper);
        owned = saturating_add(owned, static_cast<std::uint64_t>(path.native().capacity()));
        owned = saturating_add(owned, static_cast<std::uint64_t>(executable_bytes.capacity()));
        owned = saturating_add(owned, static_cast<std::uint64_t>(source.capacity()));
        return owned;
    }
};

/// @brief Loads the installed helper after checking its file type, permissions, and identity.
/// Reserves memory and saves and hashes the executable bytes. Tests may select a helper
/// with BIOSEQ_GPU_ARCHIVE_HELPER only when BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY=1.
/// Returns an error on non-Apple systems or if any check or read fails.
[[nodiscard]] inline std::expected<qualified_metal_archive_helper, std::string>
load_qualified_metal_archive_helper() {
#if !defined(__APPLE__)
    return std::unexpected("Metal archive helper qualification is unavailable off Apple platforms");
#else
    std::filesystem::path path;
    std::string source;
    if (const char* env = std::getenv("BIOSEQ_GPU_ARCHIVE_HELPER");
        env != nullptr && *env != '\0') {
        const char* test_only = std::getenv("BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY");
        if (test_only == nullptr || std::string_view{test_only} != "1") {
            return std::unexpected(
                "BIOSEQ_GPU_ARCHIVE_HELPER is rejected outside explicit test-only mode");
        }
        path = env;
        source = "test-env-override";
    } else {
        Dl_info info{};
        if (::dladdr(reinterpret_cast<const void*>(&load_qualified_metal_archive_helper), &info) ==
                0 ||
            info.dli_fname == nullptr) {
            return std::unexpected(
                "cannot locate bioseq.dylib for installed archive helper qualification");
        }
        const std::filesystem::path dylib_path(info.dli_fname);
        path = dylib_path.parent_path() / "bioseq_gpu_archive_builder";
        source = "installed-sibling";
    }
    path = path.lexically_normal();
    struct stat path_status{};
    if (::lstat(path.c_str(), &path_status) != 0 || !S_ISREG(path_status.st_mode) ||
        S_ISLNK(path_status.st_mode) || (path_status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        (path_status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0 ||
        ::access(path.c_str(), X_OK) != 0) {
        return std::unexpected(shader_cache_message(
            "archive helper is not a non-symlink executable regular file: ", path.string()));
    }
    if (path_status.st_size <= 0 || static_cast<std::uint64_t>(path_status.st_size) >
                                        sealed_shader_cache_detail::k_maximum_seed_artifact_bytes) {
        return std::unexpected(std::string{"archive helper executable size is invalid"});
    }
    const std::uint64_t admission_peak = bioseq::common::saturating_add(
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(path_status.st_size),
                                       bioseq::parallelism::k_vector_preallocation_headroom_factor),
        sizeof(qualified_metal_archive_helper) + 4096u);
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
        admission_peak, &admission_failure);
    if (!permit) {
        return std::unexpected("archive helper executable memory admission denied");
    }
    auto image = sealed_shader_cache_detail::read_stable_regular_file(
        path,
        "qualified Metal archive helper",
        static_cast<std::uint64_t>(path_status.st_size),
        sealed_shader_cache_detail::k_maximum_seed_artifact_bytes);
    if (!image)
        return std::unexpected(image.error());
    struct stat after_status{};
    if (::lstat(path.c_str(), &after_status) != 0 || !S_ISREG(after_status.st_mode) ||
        S_ISLNK(after_status.st_mode) || (after_status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        (after_status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0 ||
        ::access(path.c_str(), X_OK) != 0 ||
        sealed_shader_cache_detail::regular_file_identity_from_stat(after_status) !=
            image->identity) {
        return std::unexpected("archive helper identity changed during process qualification");
    }
    qualified_metal_archive_helper helper{
        .permit = std::move(*permit),
        .path = std::move(path),
        .identity = image->identity,
        .executable_size = static_cast<std::uint64_t>(image->bytes.size()),
        .executable_sha256 = sealed_shader_cache_detail::sha256(image->bytes),
        .executable_bytes = std::move(image->bytes),
        .source = std::move(source),
        .hash_passes = 1u,
    };
    const std::uint64_t retained = helper.owned_capacity_bytes();
    if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
        retained > helper.permit.bytes()) {
        return std::unexpected("archive helper executable ownership admission saturated");
    }
    helper.permit.shrink_to(retained);
    if (!helper.is_accepted()) {
        return std::unexpected("archive helper qualification is incomplete");
    }
    return helper;
#endif
}

/// @brief Keeps the first helper load result, including failure, for reuse within a process.
/// A mutex protects loading and access to the stored result.
class qualified_metal_archive_helper_process_store {
  public:
    /// @brief Loads the helper on first use, then returns the stored image or error.
    [[nodiscard]] std::expected<std::shared_ptr<const qualified_metal_archive_helper>, std::string>
    load_once() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (result_)
            return *result_;
        ++qualification_attempt_count_;
        auto loaded = load_qualified_metal_archive_helper();
        if (!loaded) {
            result_.emplace(std::unexpected(loaded.error()));
            return *result_;
        }
        filesystem_hash_count_ += loaded->hash_passes;
        auto retained = std::make_shared<const qualified_metal_archive_helper>(std::move(*loaded));
        result_.emplace(std::move(retained));
        return *result_;
    }

    /// @brief Returns the number of helper hashes recorded by successful loads.
    [[nodiscard]] std::uint64_t filesystem_hash_count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return filesystem_hash_count_;
    }

    /// @brief Returns helper load attempts since construction or the last test reset.
    [[nodiscard]] std::uint64_t qualification_attempt_count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return qualification_attempt_count_;
    }

    /// @brief Clears the saved result and counters when BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY=1.
    [[nodiscard]] bool reset_for_test() noexcept {
        const char* enabled = std::getenv("BIOSEQ_GPU_ARCHIVE_HELPER_TEST_ONLY");
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
    std::optional<std::expected<std::shared_ptr<const qualified_metal_archive_helper>, std::string>>
        result_;
    std::uint64_t filesystem_hash_count_ = 0u;
    std::uint64_t qualification_attempt_count_ = 0u;
};

/// @brief Returns the shared helper store used by this process.
[[nodiscard]] inline qualified_metal_archive_helper_process_store&
qualified_metal_archive_helper_process_store_instance() noexcept {
    static qualified_metal_archive_helper_process_store store;
    return store;
}

/// @brief Checks that an archive receipt names the helper image saved by this process.
/// Requires the installed helper and exactly one successful load/hash.
/// Returns an error if the executable size, hash, or source differs.
[[nodiscard]] inline std::expected<void, std::string>
validate_metal_archive_receipt_against_process_helper(
    const metal_archive_qualification_receipt& receipt) {
    if (!receipt.is_production_accepted()) {
        return std::unexpected("archive receipt is not production-qualified");
    }
    auto helper = qualified_metal_archive_helper_process_store_instance().load_once();
    if (!helper)
        return std::unexpected(helper.error());
    const qualified_metal_archive_helper& current = **helper;
    if (current.source != receipt.helper_source ||
        current.executable_size != receipt.helper_executable_size ||
        current.executable_sha256 != receipt.helper_executable_sha256 ||
        current.hash_passes != 1u ||
        qualified_metal_archive_helper_process_store_instance().filesystem_hash_count() != 1u ||
        qualified_metal_archive_helper_process_store_instance().qualification_attempt_count() !=
            1u) {
        return std::unexpected(
            "archive receipt does not match current once-qualified helper executable");
    }
    return {};
}

/// @brief Checks that a command-library receipt matches this process's saved installed helper.
/// Requires matching executable size/hash and exactly one load/hash.
[[nodiscard]] inline std::expected<void, std::string>
validate_moltenvk_command_receipt_against_process_helper(
    const moltenvk_command_library_qualification_receipt& receipt) {
    if (!receipt.is_production_accepted())
        return std::unexpected("command-library receipt is not production-qualified");
    auto helper = qualified_metal_archive_helper_process_store_instance().load_once();
    if (!helper)
        return std::unexpected(helper.error());
    const qualified_metal_archive_helper& current = **helper;
    if (current.source != receipt.helper_source ||
        current.executable_size != receipt.helper_executable_size ||
        current.executable_sha256 != receipt.helper_executable_sha256 || current.hash_passes != 1u ||
        qualified_metal_archive_helper_process_store_instance().filesystem_hash_count() != 1u ||
        qualified_metal_archive_helper_process_store_instance().qualification_attempt_count() !=
            1u) {
        return std::unexpected(
            "command-library receipt does not match current once-qualified helper executable");
    }
    return {};
}

/// @brief Loads the helper if needed and returns its original executable path.
/// Returns the saved load error on failure.
[[nodiscard]] inline std::expected<std::filesystem::path, std::string> metal_archive_helper_path() {
    auto helper = qualified_metal_archive_helper_process_store_instance().load_once();
    if (!helper)
        return std::unexpected(helper.error());
    return (*helper)->path;
}

/// @brief Checks that helper output ends with RESULT: ok followed by a newline.
/// Rejects any earlier RESULT: marker.
[[nodiscard]] inline bool
archive_helper_transcript_is_exact_success(std::string_view transcript) noexcept {
    constexpr std::string_view k_success_line = "RESULT: ok\n";
    const std::size_t result_marker = transcript.find("RESULT:");
    return transcript.size() >= k_success_line.size() &&
           transcript.substr(transcript.size() - k_success_line.size()) == k_success_line &&
           result_marker == transcript.size() - k_success_line.size();
}

/// @brief Reads the error from the helper's empty-archive pipeline test.
/// Requires one complete result line with a lowercase-hex domain and integer code.
/// Returns the decoded error or a malformed/missing-line error.
[[nodiscard]] inline std::expected<metal_archive_negative_error_fingerprint, std::string>
parse_archive_helper_negative_error_fingerprint(std::string_view transcript) {
    constexpr std::string_view k_prefix = "NEGATIVE_CONTROL_NSError domain_hex=";
    constexpr std::string_view k_code_separator = " code=";
    const std::size_t marker = transcript.find(k_prefix);
    if (marker == std::string_view::npos ||
        transcript.find(k_prefix, marker + k_prefix.size()) != std::string_view::npos ||
        (marker != 0u && transcript[marker - 1u] != '\n')) {
        return std::unexpected(
            "archive helper negative-control fingerprint is missing or repeated");
    }
    const std::size_t line_end = transcript.find('\n', marker);
    if (line_end == std::string_view::npos) {
        return std::unexpected("archive helper negative-control fingerprint line is unterminated");
    }
    const std::string_view payload =
        transcript.substr(marker + k_prefix.size(), line_end - marker - k_prefix.size());
    const std::size_t code_marker = payload.find(k_code_separator);
    if (code_marker == std::string_view::npos ||
        payload.find(k_code_separator, code_marker + k_code_separator.size()) !=
            std::string_view::npos) {
        return std::unexpected("archive helper negative-control fingerprint fields are invalid");
    }
    const std::string_view domain_hex = payload.substr(0u, code_marker);
    if (domain_hex.empty() || domain_hex.size() > 2048u || (domain_hex.size() & 1u) != 0u) {
        return std::unexpected("archive helper negative-control NSError domain hex is invalid");
    }
    std::string domain;
    domain.reserve(domain_hex.size() / 2u);
    // Decodes one lowercase hexadecimal digit.
    const auto hex_digit = [](char value) -> int {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
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
    const std::string_view code_text = payload.substr(code_marker + k_code_separator.size());
    std::int64_t code = 0;
    const auto [end, error] =
        std::from_chars(code_text.data(), code_text.data() + code_text.size(), code);
    if (error != std::errc{} || end != code_text.data() + code_text.size()) {
        return std::unexpected("archive helper negative-control NSError code is invalid");
    }
    metal_archive_negative_error_fingerprint fingerprint{
        .domain = std::move(domain),
        .code = code,
    };
    if (!fingerprint.is_accepted()) {
        return std::unexpected("archive helper negative-control NSError fingerprint is empty");
    }
    return fingerprint;
}

/// @brief Helper format version, build name, and Metal device ID reported after startup.
struct archive_helper_qualification {
    std::uint32_t wire_revision = 0u;
    std::string build_identity;
    std::uint64_t metal_registry_id = 0u;
};

/// @brief Reads the helper's build and Metal device from its output.
/// Requires one complete qualification line with the expected version and build name.
[[nodiscard]] inline std::expected<archive_helper_qualification, std::string>
parse_archive_helper_qualification(std::string_view transcript) {
    constexpr std::string_view k_prefix = "HELPER_QUALIFICATION wire_revision=";
    constexpr std::string_view k_build_separator = " build=";
    constexpr std::string_view k_registry_separator = " registry_id=";
    const std::size_t marker = transcript.find(k_prefix);
    if (marker == std::string_view::npos ||
        transcript.find(k_prefix, marker + k_prefix.size()) != std::string_view::npos ||
        (marker != 0u && transcript[marker - 1u] != '\n')) {
        return std::unexpected("archive helper qualification line is missing or repeated");
    }
    const std::size_t line_end = transcript.find('\n', marker);
    if (line_end == std::string_view::npos) {
        return std::unexpected("archive helper qualification line is unterminated");
    }
    const std::string_view payload =
        transcript.substr(marker + k_prefix.size(), line_end - marker - k_prefix.size());
    const std::size_t build_marker = payload.find(k_build_separator);
    const std::size_t registry_marker = payload.find(k_registry_separator);
    if (build_marker == std::string_view::npos || registry_marker == std::string_view::npos ||
        build_marker >= registry_marker ||
        payload.find(k_build_separator, build_marker + 1u) != std::string_view::npos ||
        payload.find(k_registry_separator, registry_marker + 1u) != std::string_view::npos) {
        return std::unexpected("archive helper qualification fields are invalid");
    }
    const std::string_view wire_revision_text = payload.substr(0u, build_marker);
    const std::string_view build =
        payload.substr(build_marker + k_build_separator.size(),
                       registry_marker - build_marker - k_build_separator.size());
    const std::string_view registry_text =
        payload.substr(registry_marker + k_registry_separator.size());
    std::uint32_t wire_revision = 0u;
    std::uint64_t registry = 0u;
    const auto [wire_revision_end, wire_revision_error] =
        std::from_chars(wire_revision_text.data(),
                        wire_revision_text.data() + wire_revision_text.size(),
                        wire_revision);
    const auto [registry_end, registry_error] = std::from_chars(
        registry_text.data(), registry_text.data() + registry_text.size(), registry);
    if (wire_revision_error != std::errc{} ||
        wire_revision_end != wire_revision_text.data() + wire_revision_text.size() ||
        registry_error != std::errc{} ||
        registry_end != registry_text.data() + registry_text.size() || wire_revision != 2u ||
        registry == 0u || build != "bioseq_gpu_archive_builder") {
        return std::unexpected("archive helper contract validation failed");
    }
    return archive_helper_qualification{
        .wire_revision = wire_revision,
        .build_identity = std::string(build),
        .metal_registry_id = registry,
    };
}

/// @brief Compiler options and device/library identities reported by command capture.
struct moltenvk_command_capture_qualification {
    std::uint32_t wire_revision = 0u;
    std::string build_identity;
    std::uint64_t capture_count = 0u;
    std::uint64_t metal_registry_id = 0u;
    moltenvk_command_compile_options compile_options{};
    moltenvk_command_vulkan_identity vulkan_device{};
    std::filesystem::path loader_image_path;
    std::filesystem::path moltenvk_image_path;

    [[nodiscard]] bool is_accepted() const noexcept {
        return wire_revision == 1u && build_identity == "bioseq_gpu_archive_builder" &&
               capture_count == 1u && metal_registry_id != 0u &&
               compile_options.is_accepted() && vulkan_device.is_accepted() &&
               loader_image_path.is_absolute() && moltenvk_image_path.is_absolute();
    }
};

/// @brief Decodes a nonempty lowercase-hex field within the given byte limit.
[[nodiscard]] inline std::expected<std::vector<std::byte>, std::string>
decode_command_capture_hex(std::string_view text, std::size_t maximum_bytes) {
    if (text.empty() || (text.size() & 1u) != 0u || text.size() / 2u > maximum_bytes)
        return std::unexpected("MoltenVK command capture hex field has invalid size");
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        return -1;
    };
    std::vector<std::byte> bytes;
    bytes.reserve(text.size() / 2u);
    for (std::size_t index = 0u; index < text.size(); index += 2u) {
        const int high = nibble(text[index]);
        const int low = nibble(text[index + 1u]);
        if (high < 0 || low < 0)
            return std::unexpected("MoltenVK command capture hex field is not lowercase hex");
        bytes.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return bytes;
}

/// @brief Reads the single COMMAND_CAPTURE line and checks all required fields.
/// Rejects duplicate or extra fields, unsupported options, and invalid device IDs or paths.
[[nodiscard]] inline std::expected<moltenvk_command_capture_qualification, std::string>
parse_moltenvk_command_capture_qualification(std::string_view transcript) {
    constexpr std::string_view k_prefix = "COMMAND_CAPTURE ";
    const std::size_t marker = transcript.find(k_prefix);
    if (marker == std::string_view::npos ||
        transcript.find(k_prefix, marker + k_prefix.size()) != std::string_view::npos ||
        (marker != 0u && transcript[marker - 1u] != '\n')) {
        return std::unexpected("MoltenVK command capture line is missing or repeated");
    }
    const std::size_t line_end = transcript.find('\n', marker);
    if (line_end == std::string_view::npos)
        return std::unexpected("MoltenVK command capture line is unterminated");
    const std::string_view payload =
        transcript.substr(marker + k_prefix.size(), line_end - marker - k_prefix.size());
    std::map<std::string_view, std::string_view> fields;
    std::size_t cursor = 0u;
    while (cursor < payload.size()) {
        const std::size_t end = payload.find(' ', cursor);
        const std::string_view token = payload.substr(
            cursor, end == std::string_view::npos ? payload.size() - cursor : end - cursor);
        const std::size_t separator = token.find('=');
        if (separator == std::string_view::npos || separator == 0u ||
            separator + 1u == token.size() ||
            !fields.emplace(token.substr(0u, separator), token.substr(separator + 1u)).second) {
            return std::unexpected("MoltenVK command capture fields are malformed or repeated");
        }
        if (end == std::string_view::npos)
            break;
        cursor = end + 1u;
    }
    constexpr std::array required_fields{
        "wire_revision",
        "build",
        "capture_count",
        "registry_id",
        "language_version",
        "fast_math_enabled",
        "math_mode_available",
        "math_mode",
        "math_fp_functions_available",
        "math_fp_functions",
        "optimization_level_available",
        "optimization_level",
        "preserve_invariance",
        "library_type_available",
        "library_type",
        "compile_symbol_visibility_available",
        "compile_symbol_visibility",
        "allow_undefined_available",
        "allow_undefined",
        "max_total_threads_available",
        "max_total_threads",
        "enable_logging_available",
        "enable_logging",
        "preprocessor_macro_count",
        "linked_library_count",
        "install_name_bytes",
        "api_version",
        "driver_version",
        "vendor_id",
        "device_id",
        "device_uuid_hex",
        "pipeline_uuid_hex",
        "loader_path_hex",
        "moltenvk_path_hex",
    };
    if (fields.size() != required_fields.size() ||
        std::any_of(required_fields.begin(), required_fields.end(), [&fields](const char* name) {
            return !fields.contains(name);
        })) {
        return std::unexpected("MoltenVK command capture field membership is not exact");
    }
    const auto number = [&fields](const char* name) -> std::expected<std::uint64_t, std::string> {
        const std::string_view text = fields.at(name);
        std::uint64_t value = 0u;
        const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size())
            return std::unexpected(shader_cache_message(
                "MoltenVK command capture numeric field is invalid: ", name));
        return value;
    };
    std::array<std::uint64_t, 28u> numbers{};
    constexpr std::array numeric_fields{
        "wire_revision",
        "capture_count",
        "registry_id",
        "language_version",
        "fast_math_enabled",
        "math_mode_available",
        "math_mode",
        "math_fp_functions_available",
        "math_fp_functions",
        "optimization_level_available",
        "optimization_level",
        "preserve_invariance",
        "library_type_available",
        "library_type",
        "compile_symbol_visibility_available",
        "compile_symbol_visibility",
        "allow_undefined_available",
        "allow_undefined",
        "max_total_threads_available",
        "max_total_threads",
        "enable_logging_available",
        "enable_logging",
        "preprocessor_macro_count",
        "linked_library_count",
        "install_name_bytes",
        "api_version",
        "driver_version",
        "vendor_id",
    };
    for (std::size_t index = 0u; index < numeric_fields.size(); ++index) {
        auto value = number(numeric_fields[index]);
        if (!value)
            return std::unexpected(value.error());
        numbers[index] = *value;
    }
    auto raw_device_id = number("device_id");
    if (!raw_device_id)
        return std::unexpected(raw_device_id.error());
    auto device_uuid = decode_command_capture_hex(fields.at("device_uuid_hex"), VK_UUID_SIZE);
    auto pipeline_uuid = decode_command_capture_hex(fields.at("pipeline_uuid_hex"), VK_UUID_SIZE);
    auto loader_path = decode_command_capture_hex(fields.at("loader_path_hex"), 4096u);
    auto moltenvk_path = decode_command_capture_hex(fields.at("moltenvk_path_hex"), 4096u);
    if (!device_uuid || !pipeline_uuid || !loader_path || !moltenvk_path ||
        device_uuid->size() != VK_UUID_SIZE || pipeline_uuid->size() != VK_UUID_SIZE) {
        return std::unexpected("MoltenVK command capture identity hex fields are invalid");
    }
    const auto path_from_bytes = [](const std::vector<std::byte>& bytes)
        -> std::expected<std::filesystem::path, std::string> {
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const std::filesystem::path path(text);
        if (text.empty() || text.find('\0') != std::string::npos || !path.is_absolute())
            return std::unexpected("MoltenVK command capture image path is invalid");
        return path;
    };
    auto loader = path_from_bytes(*loader_path);
    auto moltenvk = path_from_bytes(*moltenvk_path);
    if (!loader || !moltenvk)
        return std::unexpected(loader ? moltenvk.error() : loader.error());
    moltenvk_command_capture_qualification result{};
    if (numbers[0] > (std::numeric_limits<std::uint32_t>::max)() ||
        numbers[25] > (std::numeric_limits<std::uint32_t>::max)() ||
        numbers[26] > (std::numeric_limits<std::uint32_t>::max)() ||
        numbers[27] > (std::numeric_limits<std::uint32_t>::max)() ||
        *raw_device_id > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected("MoltenVK command capture 32-bit field overflow");
    }
    result.wire_revision = static_cast<std::uint32_t>(numbers[0]);
    result.build_identity = std::string(fields.at("build"));
    result.capture_count = numbers[1];
    result.metal_registry_id = numbers[2];
    result.compile_options = moltenvk_command_compile_options{
        .language_version = numbers[3],
        .fast_math_enabled = numbers[4],
        .math_mode_available = numbers[5],
        .math_mode = numbers[6],
        .math_fp_functions_available = numbers[7],
        .math_fp_functions = numbers[8],
        .optimization_level_available = numbers[9],
        .optimization_level = numbers[10],
        .preserve_invariance = numbers[11],
        .library_type_available = numbers[12],
        .library_type = numbers[13],
        .compile_symbol_visibility_available = numbers[14],
        .compile_symbol_visibility = numbers[15],
        .allow_undefined_available = numbers[16],
        .allow_undefined = numbers[17],
        .max_total_threads_available = numbers[18],
        .max_total_threads = numbers[19],
        .enable_logging_available = numbers[20],
        .enable_logging = numbers[21],
        .preprocessor_macro_count = numbers[22],
        .linked_library_count = numbers[23],
        .install_name_bytes = numbers[24],
    };
    result.vulkan_device.api_version = static_cast<std::uint32_t>(numbers[25]);
    result.vulkan_device.driver_version = static_cast<std::uint32_t>(numbers[26]);
    result.vulkan_device.vendor_id = static_cast<std::uint32_t>(numbers[27]);
    result.vulkan_device.device_id = static_cast<std::uint32_t>(*raw_device_id);
    std::memcpy(result.vulkan_device.device_uuid.data(), device_uuid->data(), VK_UUID_SIZE);
    std::memcpy(
        result.vulkan_device.pipeline_cache_uuid.data(), pipeline_uuid->data(), VK_UUID_SIZE);
    result.loader_image_path = std::move(*loader);
    result.moltenvk_image_path = std::move(*moltenvk);
    if (!result.is_accepted())
        return std::unexpected("MoltenVK command capture qualification is incomplete");
    return result;
}

/// @brief Descriptor number where the child process receives the shader library.
inline constexpr int k_archive_helper_metallib_child_fd = 197;

/// @brief Writes, flushes, and reads back a private file to check its complete contents.
/// The descriptor must allow reads and writes and start at offset zero. On success
/// it is rewound to zero. Returns an error if the bytes or final length differ.
[[nodiscard]] inline std::expected<void, std::string>
write_and_verify_exact_fd(int fd, std::span<const std::byte> bytes) {
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            return std::unexpected("private helper staging write failed");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
        return std::unexpected("private helper staging sync/rewind failed");
    }
    std::array<std::byte, 64u << 10u> scratch{};
    std::size_t verified = 0u;
    for (;;) {
        const ssize_t count = ::read(fd, scratch.data(), scratch.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            return std::unexpected("private helper staging verification read failed");
        }
        if (count == 0)
            break;
        const std::size_t chunk = static_cast<std::size_t>(count);
        if (verified > bytes.size() || chunk > bytes.size() - verified ||
            !std::equal(scratch.begin(),
                        scratch.begin() + static_cast<std::ptrdiff_t>(chunk),
                        bytes.begin() + static_cast<std::ptrdiff_t>(verified))) {
            return std::unexpected("private helper staging bytes differ from qualified image");
        }
        verified += chunk;
    }
    if (verified != bytes.size() || ::lseek(fd, 0, SEEK_SET) < 0) {
        return std::unexpected("private helper staging verification is incomplete");
    }
    return {};
}

/// @brief Owns one private helper copy and the optional library descriptor passed to it.
class staged_archive_helper_invocation {
  public:
    /// @brief Starts without any temporary files or descriptors.
    staged_archive_helper_invocation() = default;

    /// @brief Keeps private files and descriptors under one owner.
    staged_archive_helper_invocation(const staged_archive_helper_invocation&) = delete;

    /// @brief Prevents two objects from owning the same temporary resources.
    staged_archive_helper_invocation& operator=(const staged_archive_helper_invocation&) = delete;

    /// @brief Takes the temporary files and descriptors from `other`, leaving it empty.
    staged_archive_helper_invocation(staged_archive_helper_invocation&& other) noexcept
        : directory_(std::exchange(other.directory_, {})),
          helper_path_(std::exchange(other.helper_path_, {})),
          helper_fd_(std::exchange(other.helper_fd_, -1)),
          metallib_fd_(std::exchange(other.metallib_fd_, -1)),
          helper_identity_(other.helper_identity_),
          helper_linked_(std::exchange(other.helper_linked_, false)) {}

    /// @brief Closes descriptors and attempts to remove the private files and directory.
    ~staged_archive_helper_invocation() noexcept {
        cleanup();
    }

    /// @brief Returns the path of this invocation's private helper copy.
    /// The reference is valid until this object is moved, finished, or destroyed.
    [[nodiscard]] const std::filesystem::path& helper_path() const noexcept {
        return helper_path_;
    }
    /// @brief Returns the library descriptor to pass to the child, or -1 if none was staged.
    /// This object owns the descriptor.
    [[nodiscard]] int metallib_fd() const noexcept {
        return metallib_fd_;
    }

    /// @brief Checks that the private helper path and open descriptor still identify the staged file.
    [[nodiscard]] bool helper_identity_matches() const noexcept {
        if (helper_fd_ < 0 || helper_path_.empty() || !helper_linked_) {
            return false;
        }
        struct stat fd_status{};
        struct stat path_status{};
        return ::fstat(helper_fd_, &fd_status) == 0 &&
               ::lstat(helper_path_.c_str(), &path_status) == 0 && S_ISREG(fd_status.st_mode) &&
               S_ISREG(path_status.st_mode) && !S_ISLNK(path_status.st_mode) &&
               sealed_shader_cache_detail::regular_file_identity_from_stat(fd_status) ==
                   helper_identity_ &&
               sealed_shader_cache_detail::regular_file_identity_from_stat(path_status) ==
                   helper_identity_;
    }

    /// @brief Checks the helper's identity and removes its temporary resources.
    /// Returns the first identity or cleanup error.
    [[nodiscard]] std::expected<void, std::string> finish() {
        if (!helper_identity_matches()) {
            return std::unexpected("private archive helper identity changed across execution");
        }
        if (::unlink(helper_path_.c_str()) != 0) {
            return std::unexpected("private archive helper could not be unlinked");
        }
        helper_linked_ = false;
        struct stat absent{};
        errno = 0;
        if (::lstat(helper_path_.c_str(), &absent) == 0 || errno != ENOENT) {
            return std::unexpected("private archive helper name remained after unlink");
        }
        const int helper_fd = std::exchange(helper_fd_, -1);
        const int metallib_fd = std::exchange(metallib_fd_, -1);
        if ((helper_fd >= 0 && ::close(helper_fd) != 0) ||
            (metallib_fd >= 0 && ::close(metallib_fd) != 0)) {
            return std::unexpected("private archive helper descriptors did not close");
        }
        helper_path_.clear();
        if (::rmdir(directory_.c_str()) != 0) {
            return std::unexpected("private archive helper directory did not disappear");
        }
        directory_.clear();
        return {};
    }

  private:
    /// @brief Attempts to remove the private files and close descriptors, ignoring cleanup errors.
    void cleanup() noexcept {
        if (helper_linked_ && !helper_path_.empty()) {
            (void)::unlink(helper_path_.c_str());
        }
        if (helper_fd_ >= 0)
            (void)::close(helper_fd_);
        if (metallib_fd_ >= 0)
            (void)::close(metallib_fd_);
        if (!directory_.empty())
            (void)::rmdir(directory_.c_str());
        helper_linked_ = false;
        helper_fd_ = -1;
        metallib_fd_ = -1;
    }

    std::filesystem::path directory_;
    std::filesystem::path helper_path_;
    int helper_fd_ = -1;
    int metallib_fd_ = -1;
    sealed_shader_cache_detail::regular_file_identity helper_identity_{};
    bool helper_linked_ = false;

    friend inline std::expected<staged_archive_helper_invocation, std::string>
    stage_archive_helper_invocation(const qualified_metal_archive_helper&, int);
};

/// @brief Copies the saved helper into a private directory for one invocation.
/// Verifies the copied bytes and keeps a read-only descriptor to the executable.
/// If supplied, `metallib_fd` must be readable and is rewound and duplicated for
/// the child. Returns the owning staging object or a file/identity error.
[[nodiscard]] inline std::expected<staged_archive_helper_invocation, std::string>
stage_archive_helper_invocation(const qualified_metal_archive_helper& helper,
                                int metallib_fd = -1) {
    if (!helper.is_accepted()) {
        return std::unexpected("private archive helper staging inputs are incomplete");
    }
    std::error_code temp_error;
    const std::filesystem::path temp_parent = std::filesystem::temp_directory_path(temp_error);
    std::error_code canonical_error;
    const std::filesystem::path canonical_parent =
        temp_error ? std::filesystem::path{}
                   : std::filesystem::canonical(temp_parent, canonical_error);
    if (temp_error || canonical_error || !canonical_parent.is_absolute()) {
        return std::unexpected("private archive helper staging parent is invalid");
    }
    std::string pattern = (canonical_parent / ".bioseq-archive-helper-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* directory = ::mkdtemp(writable.data());
    if (directory == nullptr) {
        return std::unexpected("private archive helper directory creation failed");
    }
    staged_archive_helper_invocation staged;
    staged.directory_ = directory;
    if (::chmod(staged.directory_.c_str(), S_IRWXU) != 0) {
        return std::unexpected("private archive helper directory restriction failed");
    }
    struct stat directory_status{};
    if (::lstat(staged.directory_.c_str(), &directory_status) != 0 ||
        !S_ISDIR(directory_status.st_mode) || directory_status.st_uid != ::geteuid() ||
        (directory_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected("private archive helper directory identity is invalid");
    }
    staged.helper_path_ = staged.directory_ / "helper";
    staged.helper_fd_ = ::open(
        staged.helper_path_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRWXU);
    if (staged.helper_fd_ < 0) {
        return std::unexpected("private archive helper executable creation failed");
    }
    staged.helper_linked_ = true;
    if (auto exact = write_and_verify_exact_fd(staged.helper_fd_, helper.executable_bytes);
        !exact) {
        return std::unexpected(exact.error());
    }
    if (::fchmod(staged.helper_fd_, S_IRUSR | S_IXUSR) != 0) {
        return std::unexpected("private archive helper executable mode failed");
    }
    struct stat helper_status{};
    if (::fstat(staged.helper_fd_, &helper_status) != 0 || !S_ISREG(helper_status.st_mode) ||
        helper_status.st_uid != ::geteuid() || (helper_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected("private archive helper executable identity is invalid");
    }
    staged.helper_identity_ =
        sealed_shader_cache_detail::regular_file_identity_from_stat(helper_status);
    // Reopen the helper read-only before posix_spawn. Darwin may reject an
    // executable that is still open for writing. Keep this descriptor to
    // check the private file's identity around execution.
    if (::close(std::exchange(staged.helper_fd_, -1)) != 0) {
        return std::unexpected("private archive helper writable descriptor did not close");
    }
    staged.helper_fd_ = ::open(staged.helper_path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat reopened_helper_status{};
    if (staged.helper_fd_ < 0 || ::fstat(staged.helper_fd_, &reopened_helper_status) != 0 ||
        !S_ISREG(reopened_helper_status.st_mode) ||
        sealed_shader_cache_detail::regular_file_identity_from_stat(reopened_helper_status) !=
            staged.helper_identity_) {
        return std::unexpected("private archive helper read-only reopen changed identity");
    }

    if (metallib_fd < 0) {
        if (!staged.helper_identity_matches()) {
            return std::unexpected(
                "private archive staging identity changed before execution");
        }
        return staged;
    }
    if (::lseek(metallib_fd, 0, SEEK_SET) < 0) {
        return std::unexpected("qualified metallib descriptor rewind failed");
    }
    staged.metallib_fd_ =
        ::fcntl(metallib_fd, F_DUPFD_CLOEXEC, k_archive_helper_metallib_child_fd + 1);
    struct stat source_metallib{};
    struct stat inherited_metallib{};
    if (staged.metallib_fd_ < 0 || ::fstat(metallib_fd, &source_metallib) != 0 ||
        ::fstat(staged.metallib_fd_, &inherited_metallib) != 0 ||
        !S_ISREG(inherited_metallib.st_mode) ||
        sealed_shader_cache_detail::regular_file_identity_from_stat(source_metallib) !=
            sealed_shader_cache_detail::regular_file_identity_from_stat(inherited_metallib) ||
        !staged.helper_identity_matches()) {
        return std::unexpected("private archive staging identity changed before execution");
    }
    return staged;
}

/// @brief Builds the command library MoltenVK needs after a backend process forks.
/// Captures its source and options with the selected Vulkan loader, compiles them
/// with the system Metal tools, and publishes a bundle with hashes and device IDs.
/// Replaces an existing bundle in the unsealed cache. Does nothing off Apple systems.
[[nodiscard]] inline std::expected<void, std::string>
ensure_moltenvk_command_library_for_sealed_seed(const std::filesystem::path& cache_dir) {
#if !defined(__APPLE__)
    (void)cache_dir;
    return {};
#else
    std::error_code filesystem_error;
    std::filesystem::create_directories(cache_dir, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(shader_cache_message(
            "create command-library cache directory failed: ", filesystem_error.message()));
    }
    const std::filesystem::path final_bundle = cache_moltenvk_command_library_path(cache_dir);
    const std::filesystem::path temporary_source =
        cache_dir / "moltenvk_command.source.tmp.metal";
    const std::filesystem::path temporary_air = cache_dir / "moltenvk_command.air.tmp";
    const std::filesystem::path temporary_metallib =
        cache_dir / "moltenvk_command.metallib.tmp";
    const std::filesystem::path temporary_bundle = final_bundle.string() + ".tmp";
    const std::array temporary_paths{
        temporary_source, temporary_air, temporary_metallib, temporary_bundle};
    const auto cleanup_temporaries = [&temporary_paths]() noexcept {
        std::error_code ignored;
        for (const std::filesystem::path& path : temporary_paths) {
            ignored.clear();
            std::filesystem::remove(path, ignored);
        }
    };
    cleanup_temporaries();
    std::filesystem::remove(final_bundle, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(shader_cache_message(
            "remove unsealed MoltenVK command bundle failed: ", filesystem_error.message()));
    }

    filesystem_error.clear();
    const std::filesystem::path required_loader_path = selected_vulkan_loader_path(filesystem_error);
    if (filesystem_error || !required_loader_path.is_absolute()) {
        return std::unexpected("receipt-owned Vulkan loader image path is not canonical");
    }
    std::unique_ptr<void, int (*)(void*)> runtime_loader_handle(
        ::dlopen(required_loader_path.c_str(), RTLD_NOW | RTLD_LOCAL), &::dlclose);
    if (!runtime_loader_handle) {
        return std::unexpected("receipt-owned Vulkan loader image could not be opened");
    }
    void* runtime_loader_symbol =
        ::dlsym(runtime_loader_handle.get(), "vkCreateInstance");
    Dl_info runtime_loader_info{};
    if (runtime_loader_symbol == nullptr ||
        ::dladdr(runtime_loader_symbol, &runtime_loader_info) == 0 ||
        runtime_loader_info.dli_fname == nullptr) {
        return std::unexpected(
            "receipt-owned Vulkan loader does not own vkCreateInstance");
    }
    filesystem_error.clear();
    const std::filesystem::path runtime_loader_path =
        std::filesystem::canonical(runtime_loader_info.dli_fname, filesystem_error);
    if (filesystem_error || runtime_loader_path != required_loader_path) {
        return std::unexpected(
            "resolved vkCreateInstance image differs from the receipt-owned Vulkan loader");
    }
    runtime_loader_handle.reset();

    const auto helper = qualified_metal_archive_helper_process_store_instance().load_once();
    if (!helper)
        return std::unexpected(helper.error());
    const qualified_metal_archive_helper& qualified_helper = **helper;
    auto staged_helper = stage_archive_helper_invocation(qualified_helper);
    if (!staged_helper)
        return std::unexpected(staged_helper.error());
    const std::vector<std::string> capture_args{
        staged_helper->helper_path().string(),
        "--capture-moltenvk-command",
        temporary_source.string(),
        runtime_loader_path.string(),
    };
    if (!staged_helper->helper_identity_matches()) {
        cleanup_temporaries();
        return std::unexpected("private command capture helper identity changed before execution");
    }
    auto capture = run_capture(capture_args,
                               run_capture_limits{
                                   .maximum_output_bytes = 64u << 10u,
                                   .timeout = std::chrono::minutes(2),
                               });
    auto staging_finished = staged_helper->finish();
    if (!staging_finished) {
        cleanup_temporaries();
        return std::unexpected(staging_finished.error());
    }
    if (!capture) {
        cleanup_temporaries();
        return std::unexpected(
            shader_cache_message("MoltenVK command capture helper failed: ", capture.error()));
    }
    if (!archive_helper_transcript_is_exact_success(*capture)) {
        cleanup_temporaries();
        return std::unexpected(shader_cache_message(
            "MoltenVK command capture lacked exact success sentinel: ", *capture));
    }
    auto captured = parse_moltenvk_command_capture_qualification(*capture);
    if (!captured) {
        cleanup_temporaries();
        return std::unexpected(captured.error());
    }
    if (captured->loader_image_path != runtime_loader_path) {
        cleanup_temporaries();
        return std::unexpected(
            "MoltenVK command capture did not use the selected Vulkan loader image");
    }

    const auto stable_image_digest = [](const std::filesystem::path& reported,
                                        std::string_view label)
        -> std::expected<sealed_shader_cache_detail::stable_regular_file_digest, std::string> {
        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::canonical(reported, error);
        if (error || !canonical.is_absolute()) {
            return std::unexpected(shader_cache_message(
                label, " canonical image path failed: ", error.message()));
        }
        return sealed_shader_cache_detail::stream_stable_regular_file_digest(
            canonical, label, sealed_shader_cache_detail::k_maximum_seed_artifact_bytes);
    };
    auto loader_digest = stable_image_digest(captured->loader_image_path, "Vulkan loader image");
    auto moltenvk_digest = stable_image_digest(captured->moltenvk_image_path, "MoltenVK image");
    if (!loader_digest || !moltenvk_digest) {
        cleanup_temporaries();
        return std::unexpected(loader_digest ? moltenvk_digest.error() : loader_digest.error());
    }

    const std::uint64_t language_major = captured->compile_options.language_version >> 16u;
    const std::uint64_t language_minor = captured->compile_options.language_version & 0xffffu;
    if (language_major == 0u || language_major > 99u || language_minor > 99u) {
        cleanup_temporaries();
        return std::unexpected("captured MoltenVK Metal language version is not representable");
    }
    std::vector<std::string> metal_args{
        xcrun_executable(),
        "-sdk",
        "macosx",
        "metal",
        shader_cache_message("-std=metal", language_major, '.', language_minor),
    };
    if (captured->compile_options.math_mode_available != 0u) {
        constexpr std::array math_modes{
            "-fmetal-math-mode=safe",
            "-fmetal-math-mode=relaxed",
            "-fmetal-math-mode=fast",
        };
        constexpr std::array math_functions{
            "-fmetal-math-fp32-functions=fast",
            "-fmetal-math-fp32-functions=precise",
        };
        metal_args.emplace_back(math_modes[captured->compile_options.math_mode]);
        metal_args.emplace_back(
            math_functions[captured->compile_options.math_fp_functions]);
    } else {
        metal_args.emplace_back(captured->compile_options.fast_math_enabled != 0u
                                    ? "-ffast-math"
                                    : "-fno-fast-math");
    }
    if (captured->compile_options.optimization_level == 1u)
        metal_args.emplace_back("-Os");
    if (captured->compile_options.preserve_invariance != 0u)
        metal_args.emplace_back("-fpreserve-invariance");
    metal_args.emplace_back("-c");
    metal_args.push_back(temporary_source.string());
    metal_args.emplace_back("-o");
    metal_args.push_back(temporary_air.string());
    if (auto compiled = spawn_wait(metal_args); !compiled) {
        cleanup_temporaries();
        return std::unexpected(
            shader_cache_message("exact MoltenVK command source compile failed: ", compiled.error()));
    }
    const std::vector<std::string> metallib_args{
        xcrun_executable(),
        "-sdk",
        "macosx",
        "metallib",
        temporary_air.string(),
        "-o",
        temporary_metallib.string(),
    };
    if (auto linked = spawn_wait(metallib_args); !linked) {
        cleanup_temporaries();
        return std::unexpected(shader_cache_message(
            "exact MoltenVK command metallib link failed: ", linked.error()));
    }

    const auto read_stable = [](const std::filesystem::path& path,
                                std::string_view label,
                                std::uint64_t maximum)
        -> std::expected<std::vector<std::byte>, std::string> {
        struct stat status{};
        if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
            S_ISLNK(status.st_mode) || status.st_size <= 0 ||
            static_cast<std::uint64_t>(status.st_size) > maximum) {
            return std::unexpected(shader_cache_message(label, " identity is invalid"));
        }
        auto image = sealed_shader_cache_detail::read_stable_regular_file(
            path, label, static_cast<std::uint64_t>(status.st_size), maximum);
        if (!image)
            return std::unexpected(image.error());
        return std::move(image->bytes);
    };
    auto source = read_stable(temporary_source,
                              "captured MoltenVK command source",
                              sealed_shader_cache_detail::k_maximum_command_source_bytes);
    auto metallib = read_stable(temporary_metallib,
                                "compiled MoltenVK command metallib",
                                sealed_shader_cache_detail::k_maximum_seed_artifact_bytes);
    if (!source || !metallib) {
        cleanup_temporaries();
        return std::unexpected(source ? metallib.error() : source.error());
    }
    moltenvk_command_library_bundle bundle{};
    bundle.source = std::move(*source);
    bundle.metallib = std::move(*metallib);
    bundle.qualification = moltenvk_command_library_qualification_receipt{
        .wire_revision = sealed_shader_cache_detail::k_command_library_wire_revision,
        .capture_count = captured->capture_count,
        .source_size = static_cast<std::uint64_t>(bundle.source.size()),
        .source_sha256 = sealed_shader_cache_detail::sha256(bundle.source),
        .compile_options = captured->compile_options,
        .source_options_sha256 = moltenvk_command_source_options_fingerprint(
            bundle.source, captured->compile_options),
        .metallib_size = static_cast<std::uint64_t>(bundle.metallib.size()),
        .metallib_sha256 = sealed_shader_cache_detail::sha256(bundle.metallib),
        .helper_wire_revision = 2u,
        .helper_build_identity = captured->build_identity,
        .helper_source = qualified_helper.source,
        .helper_executable_size = qualified_helper.executable_size,
        .helper_executable_sha256 = qualified_helper.executable_sha256,
        .helper_metal_registry_id = captured->metal_registry_id,
        .vulkan_device = captured->vulkan_device,
        .loader_image_size = loader_digest->size,
        .loader_image_sha256 = loader_digest->sha256,
        .moltenvk_image_size = moltenvk_digest->size,
        .moltenvk_image_sha256 = moltenvk_digest->sha256,
    };
    if (!bundle.is_accepted()) {
        cleanup_temporaries();
        return std::unexpected("exact MoltenVK command-library bundle failed self-validation");
    }
    auto wire = sealed_shader_cache_detail::serialize_moltenvk_command_library_bundle(bundle);
    if (!wire) {
        cleanup_temporaries();
        return std::unexpected(wire.error());
    }
    if (auto written = durable_write_file(temporary_bundle, *wire); !written) {
        cleanup_temporaries();
        return written;
    }
    if (auto published = durable_rename_existing(temporary_bundle, final_bundle); !published) {
        cleanup_temporaries();
        return published;
    }
    cleanup_temporaries();
    return {};
#endif
}

/// @brief Builds missing Metal pipeline archives and records their tested inputs.
/// The helper checks that each archive creates the requested pipeline and that
/// an empty archive fails. Existing nonempty archive/receipt pairs are reused
/// unless `force_requalify` is set.
/// @param[in] meta Validated shader metadata used for workgroup settings.
/// @param[in] entry_name Metal function to include in each archive.
/// @return Success or the first helper, file, or validation error.
[[nodiscard]] inline std::expected<void, std::string>
ensure_binary_archive(const std::filesystem::path& cache_dir,
                      const shader_variant_key& key,
                      const shader_cache_metadata& meta,
                      std::string_view entry_name,
                      bool force_requalify = false) {
#if !defined(__APPLE__)
    (void)cache_dir;
    (void)key;
    (void)meta;
    (void)entry_name;
    (void)force_requalify;
    return {};
#else
    if (entry_name.empty()) {
        return std::unexpected(shader_cache_message("cannot build MTLBinaryArchive for ",
                                                    cache_file_stem(key),
                                                    ": empty Metal entry name"));
    }
    std::error_code ec;
    if (force_requalify) {
        for (const std::filesystem::path& final_path : cache_binary_archive_paths(cache_dir, key)) {
            std::filesystem::remove(final_path, ec);
            if (ec) {
                return std::unexpected(shader_cache_message(
                    "remove unsealed MTLBinaryArchive before qualification failed for ",
                    cache_file_stem(key),
                    ": ",
                    ec.message()));
            }
            std::filesystem::remove(final_path.string() + ".tmp", ec);
            if (ec) {
                return std::unexpected(
                    shader_cache_message("remove stale MTLBinaryArchive temporary failed for ",
                                         cache_file_stem(key),
                                         ": ",
                                         ec.message()));
            }
            const std::filesystem::path qualification_path =
                cache_binary_archive_qualification_path(final_path);
            std::filesystem::remove(qualification_path, ec);
            if (ec) {
                return std::unexpected(shader_cache_message(
                    "remove unsealed MTLBinaryArchive qualification failed for ",
                    cache_file_stem(key),
                    ": ",
                    ec.message()));
            }
            std::filesystem::remove(qualification_path.string() + ".tmp", ec);
            if (ec) {
                return std::unexpected(shader_cache_message(
                    "remove stale MTLBinaryArchive qualification temporary failed for ",
                    cache_file_stem(key),
                    ": ",
                    ec.message()));
            }
        }
    }
    const auto helper = qualified_metal_archive_helper_process_store_instance().load_once();
    if (!helper) {
        return std::unexpected(helper.error());
    }
    const qualified_metal_archive_helper& qualified_helper = **helper;
    const std::filesystem::path metallib_path = cache_metallib_path(cache_dir, key);
    const int metallib_fd = ::open(metallib_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (metallib_fd < 0) {
        return std::unexpected("qualified Metal shader library open failed");
    }
    capture_file_descriptor metallib_fd_owner{metallib_fd};
    struct stat metallib_before{};
    if (::fstat(metallib_fd, &metallib_before) != 0 || !S_ISREG(metallib_before.st_mode) ||
        metallib_before.st_size <= 0 ||
        static_cast<std::uint64_t>(metallib_before.st_size) >
            sealed_shader_cache_detail::k_maximum_seed_artifact_bytes) {
        return std::unexpected("qualified Metal shader library identity is invalid");
    }
    sealed_shader_cache_detail::sha256_hasher metallib_hasher;
    std::array<std::byte, 64u << 10u> metallib_scratch{};
    std::uint64_t metallib_size = 0u;
    for (;;) {
        const ssize_t count = ::read(metallib_fd, metallib_scratch.data(), metallib_scratch.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            return std::unexpected("qualified Metal shader library hash read failed");
        }
        if (count == 0)
            break;
        const std::size_t chunk = static_cast<std::size_t>(count);
        metallib_hasher.update(std::span<const std::byte>{metallib_scratch.data(), chunk});
        metallib_size += static_cast<std::uint64_t>(chunk);
    }
    struct stat metallib_after{};
    if (::fstat(metallib_fd, &metallib_after) != 0 ||
        sealed_shader_cache_detail::regular_file_identity_from_stat(metallib_before) !=
            sealed_shader_cache_detail::regular_file_identity_from_stat(metallib_after) ||
        metallib_size != static_cast<std::uint64_t>(metallib_after.st_size) ||
        ::lseek(metallib_fd, 0, SEEK_SET) < 0) {
        return std::unexpected("qualified Metal shader library changed during hash");
    }
    const auto metallib_sha256 = metallib_hasher.finalize();

    const std::uint64_t shader_threads =
        static_cast<std::uint64_t>(std::max<std::uint32_t>(meta.wg_x, 1u)) *
        static_cast<std::uint64_t>(std::max<std::uint32_t>(meta.wg_y, 1u)) *
        static_cast<std::uint64_t>(std::max<std::uint32_t>(meta.wg_z, 1u));
    // MoltenVK can use a 256-thread device limit in its pipeline descriptor.
    // Build archives for that limit and for the shader's workgroup size so a
    // forked backend can reuse either descriptor without compiling a pipeline.
    const std::vector<std::uint64_t> archive_thread_caps = [](std::uint64_t exact_threads) {
        constexpr std::uint64_t k_conservative_moltenvk_cap = 256u;
        std::vector<std::uint64_t> caps{exact_threads};
        if (exact_threads != k_conservative_moltenvk_cap) {
            caps.push_back(k_conservative_moltenvk_cap);
        }
        return caps;
    }(shader_threads);

    for (std::size_t cap_index = 0; cap_index < archive_thread_caps.size(); ++cap_index) {
        const std::uint64_t max_threads = archive_thread_caps[cap_index];
        const std::filesystem::path final_path =
            cap_index == 0 ? cache_binary_archive_path(cache_dir, key)
                           : cache_dir / (cache_file_stem(key) + ".max_threads_" +
                                          std::to_string(max_threads) + ".metalar");
        const std::filesystem::path qualification_path =
            cache_binary_archive_qualification_path(final_path);
        if (!force_requalify && std::filesystem::exists(final_path, ec) && !ec &&
            std::filesystem::file_size(final_path, ec) > 0 && !ec &&
            std::filesystem::exists(qualification_path, ec) && !ec &&
            std::filesystem::file_size(qualification_path, ec) > 0 && !ec) {
            continue;
        }
        ec.clear();
        const std::filesystem::path archive_temporary_path = final_path.string() + ".tmp";
        const std::filesystem::path qualification_temporary_path =
            qualification_path.string() + ".tmp";
        std::filesystem::remove(archive_temporary_path, ec);
        ec.clear();
        std::filesystem::remove(qualification_temporary_path, ec);
        ec.clear();

        auto staged_helper = stage_archive_helper_invocation(qualified_helper, metallib_fd);
        if (!staged_helper) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return std::unexpected(staged_helper.error());
        }
        const std::vector<std::string> args{
            staged_helper->helper_path().string(),
            "/dev/fd/" + std::to_string(k_archive_helper_metallib_child_fd),
            archive_temporary_path.string(),
            std::string(entry_name),
            std::to_string(max_threads),
            "0",
        };
        if (!staged_helper->helper_identity_matches()) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return std::unexpected("private archive helper identity changed before execution");
        }
        const std::array inherited_fds{
            run_capture_inherited_fd{
                .source_fd = staged_helper->metallib_fd(),
                .target_fd = k_archive_helper_metallib_child_fd,
            },
        };
        auto result = run_capture(args,
                                  run_capture_limits{
                                      .maximum_output_bytes = 64u << 10u,
                                      .timeout = std::chrono::minutes(2),
                                  },
                                  inherited_fds);
        struct stat metallib_after_execution{};
        const bool metallib_identity_unchanged =
            ::fstat(metallib_fd, &metallib_after_execution) == 0 &&
            sealed_shader_cache_detail::regular_file_identity_from_stat(metallib_after_execution) ==
                sealed_shader_cache_detail::regular_file_identity_from_stat(metallib_after);
        auto staging_finished = staged_helper->finish();
        if (!staging_finished) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return std::unexpected(staging_finished.error());
        }
        if (!metallib_identity_unchanged) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return std::unexpected(
                "qualified metallib descriptor identity changed during helper execution");
        }
        if (!result) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper failed for ", cache_file_stem(key), ": ", result.error()));
        }
        if (!archive_helper_transcript_is_exact_success(*result)) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper returned without exact success sentinel for ",
                cache_file_stem(key),
                ": ",
                *result));
        }
        auto negative_error = parse_archive_helper_negative_error_fingerprint(*result);
        if (!negative_error) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper returned an invalid negative-control result for ",
                cache_file_stem(key),
                ": ",
                negative_error.error()));
        }
        auto helper_qualification = parse_archive_helper_qualification(*result);
        if (!helper_qualification) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return std::unexpected(shader_cache_message(
                "MTLBinaryArchive helper returned invalid qualification provenance for ",
                cache_file_stem(key),
                ": ",
                helper_qualification.error()));
        }
        if (!std::filesystem::exists(archive_temporary_path, ec) || ec ||
            std::filesystem::file_size(archive_temporary_path, ec) == 0 || ec) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return std::unexpected(
                shader_cache_message("MTLBinaryArchive helper produced no archive for ",
                                     cache_file_stem(key),
                                     ": ",
                                     *result));
        }
        auto archive_digest = sealed_shader_cache_detail::stream_stable_regular_file_digest(
            archive_temporary_path,
            "qualified Metal binary archive",
            sealed_shader_cache_detail::k_maximum_seed_artifact_bytes);
        if (!archive_digest) {
            std::filesystem::remove(archive_temporary_path, ec);
            return std::unexpected(archive_digest.error());
        }
        const metal_archive_qualification_receipt receipt{
            .wire_revision = sealed_shader_cache_detail::k_archive_qualification_wire_revision,
            .shader_key = key,
            .metallib_size = metallib_size,
            .metallib_sha256 = metallib_sha256,
            .entry_name = std::string(entry_name),
            .max_total_threads_per_threadgroup = max_threads,
            .threadgroup_size_is_multiple = false,
            .negative_error = std::move(*negative_error),
            .archive_size = archive_digest->size,
            .archive_sha256 = archive_digest->sha256,
            .helper_wire_revision = helper_qualification->wire_revision,
            .helper_build_identity = std::move(helper_qualification->build_identity),
            .helper_source = qualified_helper.source,
            .helper_executable_size = qualified_helper.executable_size,
            .helper_executable_sha256 = qualified_helper.executable_sha256,
            .helper_metal_registry_id = helper_qualification->metal_registry_id,
        };
        auto receipt_bytes =
            sealed_shader_cache_detail::serialize_metal_archive_qualification_receipt(receipt);
        if (!receipt_bytes) {
            std::filesystem::remove(archive_temporary_path, ec);
            return std::unexpected(receipt_bytes.error());
        }
        if (auto written = durable_write_file(qualification_temporary_path, *receipt_bytes);
            !written) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return written;
        }
        if (!sealed_shader_cache_detail::same_regular_file_identity(archive_temporary_path,
                                                                    archive_digest->identity)) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return std::unexpected("qualified Metal binary archive changed before publication");
        }
        if (auto renamed = durable_rename_existing(archive_temporary_path, final_path); !renamed) {
            std::filesystem::remove(archive_temporary_path, ec);
            std::filesystem::remove(qualification_temporary_path, ec);
            return renamed;
        }
        if (auto renamed =
                durable_rename_existing(qualification_temporary_path, qualification_path);
            !renamed) {
            std::filesystem::remove(qualification_temporary_path, ec);
            return renamed;
        }
    }
    return {};
#endif
}

} // namespace bioseq::gpu::vulkan

/**
 * @file archive_helper.cppm
 * @brief Owns Metal archive qualification, helper staging, and cache-entry status.
 */
module;

#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module bioseq.gpu.vulkan.shader_cache:archive_helper;

import :paths_metadata;
import :subprocess;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import std;

export namespace bioseq::gpu::vulkan {

/// @brief Stable NSError identity captured from the archive negative control.
struct metal_archive_negative_error_fingerprint {
    std::string domain;
    std::int64_t code = 0;

    /// @brief Reports whether the NSError domain is bounded and nonempty.
    [[nodiscard]] bool is_accepted() const noexcept {
        return !domain.empty() && domain.size() <= 1024u && domain.find('\0') == std::string::npos;
    }
    /// @brief Compares the NSError domain and code.
    [[nodiscard]] bool
    operator==(const metal_archive_negative_error_fingerprint&) const noexcept = default;
};

/// @brief Contract record binding one Metal binary archive to its helper and metallib.
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

    /// @brief Validates fields required to report an archive qualification attempt.
    [[nodiscard]] bool is_diagnostic_valid() const noexcept {
        return wire_revision == 2u && max_total_threads_per_threadgroup > 0u &&
               max_total_threads_per_threadgroup <= 1024u && negative_error.is_accepted() &&
               archive_size != 0u &&
               std::any_of(archive_sha256.begin(), archive_sha256.end(), [](std::uint8_t byte) {
                   return byte != 0u;
               });
    }
    /// @brief Validates complete archive, metallib, helper, and entrypoint identity.
    [[nodiscard]] bool is_accepted() const noexcept {
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
    /// @brief Requires an accepted receipt from the installed sibling helper.
    [[nodiscard]] bool is_production_accepted() const noexcept {
        return is_accepted() && helper_source == "installed-sibling" &&
               helper_build_identity == "bioseq_gpu_archive_builder";
    }
    /// @brief Compares every serialized qualification field.
    [[nodiscard]] bool
    operator==(const metal_archive_qualification_receipt&) const noexcept = default;
};

/// @brief Validates every required Metal archive and qualification receipt.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @return Success or the first incomplete archive diagnostic.
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

/// @brief Reports whether every required Metal archive is present and qualified.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @return `true` when the archive set is complete.
[[nodiscard]] inline bool cache_binary_archive_complete(const std::filesystem::path& cache_dir,
                                                        const shader_variant_key& key) {
    return cache_binary_archive_status(cache_dir, key).has_value();
}

/// @brief Validates all cache artifacts required by one shader variant.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @return Success or the first incomplete artifact diagnostic.
[[nodiscard]] inline std::expected<void, std::string>
cache_entry_status(const std::filesystem::path& cache_dir, const shader_variant_key& key) {
    if (auto metallib = cache_metallib_entry_status(cache_dir, key); !metallib) {
        return metallib;
    }
    return cache_binary_archive_status(cache_dir, key);
}

/// @brief Reports whether one shader variant has a complete cache entry.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @return `true` when metadata, library, and archives are complete.
[[nodiscard]] inline bool cache_entry_complete(const std::filesystem::path& cache_dir,
                                               const shader_variant_key& key) {
    return cache_entry_status(cache_dir, key).has_value();
}

namespace sealed_shader_cache_detail {

/// @brief Qualification record magic stored in every archive receipt.
inline constexpr std::string_view k_archive_qualification_magic = "BAMISS01";
/// @brief Current archive qualification wire revision.
inline constexpr std::uint32_t k_archive_qualification_wire_revision = 2u;
/// @brief Maximum accepted archive qualification record size.
inline constexpr std::size_t k_maximum_archive_qualification_bytes = 4096u;

/// @brief Serializes one accepted Metal archive qualification receipt.
/// @param[in] receipt Qualification receipt.
/// @return Serialized receipt bytes or a contract diagnostic.
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

/// @brief Parses and validates one Metal archive qualification receipt.
/// @param[in] bytes Serialized receipt bytes.
/// @return Accepted receipt or a wire-format diagnostic.
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

} // namespace sealed_shader_cache_detail

/// @brief Stable process-owned image and identity of the archive helper executable.
struct qualified_metal_archive_helper {
    bioseq::parallelism::memory_permit permit;
    std::filesystem::path path;
    sealed_shader_cache_detail::regular_file_identity identity{};
    std::uint64_t executable_size = 0u;
    std::array<std::uint8_t, 32> executable_sha256{};
    std::vector<std::byte> executable_bytes;
    std::string source;
    std::uint64_t hash_passes = 0u;

    /// @brief Validates executable identity, retained bytes, and admission accounting.
    [[nodiscard]] bool is_accepted() const noexcept {
        return permit && permit.bytes() == owned_capacity_bytes() && !path.empty() &&
               executable_size != 0u && hash_passes == 1u &&
               executable_bytes.size() == executable_size &&
               (source == "installed-sibling" || source == "test-env-override") &&
               std::any_of(executable_sha256.begin(),
                           executable_sha256.end(),
                           [](std::uint8_t byte) { return byte != 0u; });
    }

    /// @brief Computes storage retained by the helper image.
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::common::saturating_add;
        std::uint64_t owned = sizeof(qualified_metal_archive_helper);
        owned = saturating_add(owned, static_cast<std::uint64_t>(path.native().capacity()));
        owned = saturating_add(owned, static_cast<std::uint64_t>(executable_bytes.capacity()));
        owned = saturating_add(owned, static_cast<std::uint64_t>(source.capacity()));
        return owned;
    }
};

/// @brief Locates, validates, reads, hashes, and admits the archive helper executable.
/// @return Qualified helper image or a platform, identity, admission, or I/O diagnostic.
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

/// @brief Loads and retains exactly one archive helper qualification per process.
class qualified_metal_archive_helper_process_store {
  public:
    /// @brief Loads and hashes the qualified helper at most once.
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

    /// @brief Returns completed helper filesystem hashes.
    [[nodiscard]] std::uint64_t filesystem_hash_count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return filesystem_hash_count_;
    }

    /// @brief Returns process helper qualification attempts.
    [[nodiscard]] std::uint64_t qualification_attempt_count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return qualification_attempt_count_;
    }

    /// @brief Clears the helper store only when the explicit test seam is enabled.
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

/// @brief Returns the process-owned archive helper store.
[[nodiscard]] inline qualified_metal_archive_helper_process_store&
qualified_metal_archive_helper_process_store_instance() noexcept {
    static qualified_metal_archive_helper_process_store store;
    return store;
}

/// @brief Matches a production archive receipt to the once-qualified process helper.
/// @param[in] receipt Archive qualification receipt.
/// @return Success or a helper identity diagnostic.
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

/// @brief Returns the once-qualified archive helper executable path.
/// @return Qualified helper path or the qualification diagnostic.
[[nodiscard]] inline std::expected<std::filesystem::path, std::string> metal_archive_helper_path() {
    auto helper = qualified_metal_archive_helper_process_store_instance().load_once();
    if (!helper)
        return std::unexpected(helper.error());
    return (*helper)->path;
}

/// @brief Requires one unique terminal archive-helper success sentinel.
/// @param[in] transcript Complete helper output.
/// @return `true` when the final line is the sole result marker.
[[nodiscard]] inline bool
archive_helper_transcript_is_exact_success(std::string_view transcript) noexcept {
    constexpr std::string_view k_success_line = "RESULT: ok\n";
    const std::size_t result_marker = transcript.find("RESULT:");
    return transcript.size() >= k_success_line.size() &&
           transcript.substr(transcript.size() - k_success_line.size()) == k_success_line &&
           result_marker == transcript.size() - k_success_line.size();
}

/// @brief Parses the exact NSError fingerprint emitted by the archive negative control.
/// @param[in] transcript Complete helper output.
/// @return Accepted fingerprint or a transcript diagnostic.
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

/// @brief Helper wire revision, build identity, and Metal registry identity.
struct archive_helper_qualification {
    std::uint32_t wire_revision = 0u;
    std::string build_identity;
    std::uint64_t metal_registry_id = 0u;
};

/// @brief Parses archive helper qualification from the complete transcript.
/// @param[in] transcript Complete helper output.
/// @return Accepted qualification or a transcript diagnostic.
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

/// @brief Fixed descriptor through which the helper reads the staged metallib.
inline constexpr int k_archive_helper_metallib_child_fd = 197;

/// @brief Writes exact bytes to a descriptor and verifies its final size and offset.
/// @param[in] fd Open writable descriptor.
/// @param[in] bytes Expected complete payload.
/// @return Success or an I/O or size diagnostic.
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

/// @brief Owns a private helper executable and inherited metallib descriptor.
class staged_archive_helper_invocation {
  public:
    /// @brief Creates an empty staging owner.
    staged_archive_helper_invocation() = default;

    /// @brief Prevents copying private helper paths and descriptors.
    staged_archive_helper_invocation(const staged_archive_helper_invocation&) = delete;

    /// @brief Prevents copy assignment of private helper resources.
    staged_archive_helper_invocation& operator=(const staged_archive_helper_invocation&) = delete;

    /// @brief Transfers the private directory, helper, descriptors, and identity.
    /// @param[in,out] other Staging owner whose resources are consumed.
    staged_archive_helper_invocation(staged_archive_helper_invocation&& other) noexcept
        : directory_(std::exchange(other.directory_, {})),
          helper_path_(std::exchange(other.helper_path_, {})),
          helper_fd_(std::exchange(other.helper_fd_, -1)),
          metallib_fd_(std::exchange(other.metallib_fd_, -1)),
          helper_identity_(other.helper_identity_),
          helper_linked_(std::exchange(other.helper_linked_, false)) {}

    /// @brief Removes any staged path and closes retained descriptors.
    ~staged_archive_helper_invocation() noexcept {
        cleanup();
    }

    /// @brief Returns the private helper executable path.
    [[nodiscard]] const std::filesystem::path& helper_path() const noexcept {
        return helper_path_;
    }
    /// @brief Returns the staged metallib descriptor inherited by the child.
    [[nodiscard]] int metallib_fd() const noexcept {
        return metallib_fd_;
    }

    /// @brief Checks that the linked helper and retained descriptor still match.
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

    /// @brief Verifies, unlinks, closes, and removes all staged resources.
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
        if (::close(std::exchange(helper_fd_, -1)) != 0 ||
            ::close(std::exchange(metallib_fd_, -1)) != 0) {
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
    /// @brief Best-effort cleanup used by destruction and failed staging paths.
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

/// @brief Stages the qualified helper and a read-only metallib descriptor privately.
/// @param[in] helper Once-qualified helper executable image.
/// @param[in] metallib_fd Open metallib descriptor to duplicate.
/// @return Move-owned invocation or an identity, permission, or I/O diagnostic.
[[nodiscard]] inline std::expected<staged_archive_helper_invocation, std::string>
stage_archive_helper_invocation(const qualified_metal_archive_helper& helper, int metallib_fd) {
    if (!helper.is_accepted() || metallib_fd < 0) {
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
    // Do not carry a writable executable descriptor across posix_spawn.
    // Darwin may reject execution while the vnode is still open for writing,
    // and the retained qualification should hold only a non-mutating descriptor.
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

/// @brief Builds and qualifies every Metal archive required by one shader variant.
/// @param[in] cache_dir Shader-cache directory.
/// @param[in] key Shader variant identity.
/// @param[in] meta Validated shader metadata.
/// @param[in] entry_name Metal entrypoint name.
/// @param[in] force_requalify Whether existing archives must be replaced.
/// @return Success or the first helper, archive, qualification, or I/O diagnostic.
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
    // MoltenVK may create the Metal pipeline descriptor with the conservative
    // device cap rather than the GLSL local size. Keep both archives so a
    // forked backend can match the descriptor without invoking MTLCompilerService.
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

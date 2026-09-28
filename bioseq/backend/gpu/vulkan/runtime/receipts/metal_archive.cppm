/**
 * @file metal_archive.cppm
 * @brief Records Metal archive settings and checks used to accept GPU pipelines.
 */
module;

#include <cstddef>
#include <cstdint>
#include <libassert/assert.hpp>
#include <new>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:receipts.metal_archive;

import :receipts.core;
import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.gpu.vulkan.abi;
import bioseq.prefilter.types;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.instrumentation;
import std;

export namespace bioseq::gpu::vulkan {

/// Linux accepts native Vulkan pipeline records without Metal archive checks.
#if defined(__linux__)
inline constexpr bool k_native_vulkan_pipeline_receipts = true;
#else
inline constexpr bool k_native_vulkan_pipeline_receipts = false;
#endif

/// @brief Identifies one Metal binary archive in the loaded shader-cache seed.
/// The artifact index selects bytes already kept in memory, so replacing
/// a cache file later cannot change the archive used by the runtime.
struct runtime_metal_archive_spec {
    std::uint32_t artifact_ordinal = (std::numeric_limits<std::uint32_t>::max)(); ///< Seed-image artifact index.
    std::string artifact_filename;                                                ///< Archive filename recorded in the seed.
    std::uint64_t size = 0u;                                                      ///< Archive size  in bytes.
    std::array<std::uint8_t, 32> sha256{};                                        ///< Archive SHA-256 digest.
    std::string negative_error_domain;                                            ///< Expected error domain when the pipline is tested with an empty archive.
    std::int64_t negative_error_code = 0;                                         ///< Expected error code from the empty-archive test.

    /// @brief Checks that the archive index, filename, size, digest, and expected error domain are populated.
    [[nodiscard]] bool is_valid() const noexcept {
        return artifact_ordinal != (std::numeric_limits<std::uint32_t>::max)() &&
               !artifact_filename.empty() && size != 0u && !negative_error_domain.empty() &&
               negative_error_domain.size() <= 1024u &&
               negative_error_domain.find('\0') == std::string::npos &&
               std::any_of(
                   sha256.begin(), sha256.end(), [](std::uint8_t byte) { return byte != 0u; });
    }

    /// @brief Compares all archive details, including the expected empty-archive error.
    [[nodiscard]] bool operator==(const runtime_metal_archive_spec&) const noexcept = default;
};

/// @brief Shader-cache contents selected before the Vulkan runtime starts.
/// Lists the, accepted archives explicitly. The runtime loads their saved
/// seed bytes without searching a directory for other archives.
struct runtime_metal_archive_configuration {
    std::shared_ptr<const sealed_shader_cache_seed_image> seed_image; ///< Accepted  shader-cache seed kept in memory.
    std::uint32_t command_artifact_ordinal =
        (std::numeric_limits<std::uint32_t>::max)(); ///< Index of the MoltenVK command-library bundle in the seed.
    moltenvk_command_library_bundle command_library; ///< MoltenVK command source, compiler options, and precompiled library.
    /// @brief Optional device checks supplied by the caller.
    /// Zero lets the runtime select the device A supplied identifier must match
    /// the device selected before vkCreateDevice.
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid{}; ///< Optional Vulkan device UUID to require.
    std::uint64_t metal_device_registry_id = 0u;                 ///< Optional Metal device registry ID to require.
    std::uint32_t qualification_wire_revision = 0u;              ///< Expected archive-check record forat version.
    std::uint64_t qualification_receipt_count = 0u;            ///< Expected number of archive-check records.
    std::uint32_t helper_wire_revision = 0u;                   ///< Expected archive-builder  record format version.
    std::string helper_build_identity;                           ///< Expected archive-builder executable identity.
    std::string helper_source;                                   ///< Expected source of the archive-builder executable.
    std::uint64_t helper_executable_size = 0u;                 ///< Expected helper executable size in bytes.
    std::array<std::uint8_t, 32> helper_executable_sha256{};     ///< Expected helper executable SHA-256 digest.
    std::uint64_t helper_metal_registry_id = 0u;                 ///< Metal device registry ID used by the archive builder.
    std::vector<runtime_metal_archive_spec> archives;              ///< Archives in their order within the seed.

    /// @brief Checks that archive and command-library records match the accepted seed.
    /// Also requires the same archive builder and device details across the records.
    [[nodiscard]] bool is_valid() const noexcept {
        if (!seed_image || !seed_image->is_accepted() || archives.empty() ||
            command_artifact_ordinal == (std::numeric_limits<std::uint32_t>::max)() ||
            !command_library.is_accepted() ||
            !command_library.qualification.is_production_accepted() ||
            qualification_wire_revision != 2u || qualification_receipt_count != archives.size() ||
            helper_wire_revision != 2u || helper_build_identity != "bioseq_gpu_archive_builder" ||
            helper_source != "installed-sibling" || helper_executable_size == 0u ||
            helper_metal_registry_id == 0u ||
            std::all_of(helper_executable_sha256.begin(),
                        helper_executable_sha256.end(),
                        [](std::uint8_t value) { return value == 0u; }) ||
            command_library.qualification.helper_wire_revision != helper_wire_revision ||
            command_library.qualification.helper_build_identity != helper_build_identity ||
            command_library.qualification.helper_source != helper_source ||
            command_library.qualification.helper_executable_size != helper_executable_size ||
            command_library.qualification.helper_executable_sha256 != helper_executable_sha256 ||
            command_library.qualification.helper_metal_registry_id != helper_metal_registry_id ||
            (metal_device_registry_id != 0u &&
             metal_device_registry_id != helper_metal_registry_id) ||
            (std::any_of(vulkan_device_uuid.begin(),
                         vulkan_device_uuid.end(),
                         [](std::uint8_t value) { return value != 0u; }) &&
             vulkan_device_uuid != command_library.qualification.vulkan_device.device_uuid) ||
            !is_runtime_cache_digest_valid(seed_image->manifest().cache_digest)) {
            return false;
        }
        try {
            const sealed_shader_cache_artifact_image* command_image =
                seed_image->artifact(command_artifact_ordinal);
            if (command_image == nullptr ||
                command_image->descriptor.kind !=
                    sealed_shader_cache_artifact_kind::moltenvk_command_library ||
                command_image->descriptor.key_index !=
                    (std::numeric_limits<std::uint32_t>::max)() ||
                command_image->descriptor.filename != "moltenvk_command.sealed") {
                return false;
            }
            auto parsed_command =
                sealed_shader_cache_detail::parse_moltenvk_command_library_bundle(
                    command_image->bytes);
            if (!parsed_command || *parsed_command != command_library)
                return false;
            for (std::size_t i = 0u; i < archives.size(); ++i) {
                const runtime_metal_archive_spec& archive = archives[i];
                const sealed_shader_cache_artifact_image* image =
                    seed_image->artifact(archive.artifact_ordinal);
                if (!archive.is_valid() || image == nullptr ||
                    image->descriptor.kind !=
                        sealed_shader_cache_artifact_kind::metal_binary_archive ||
                    image->descriptor.filename != archive.artifact_filename ||
                    image->descriptor.size != archive.size ||
                    image->descriptor.sha256 != archive.sha256 ||
                    (i != 0u && archives[i - 1u].artifact_ordinal >= archive.artifact_ordinal) ||
                    (i != 0u &&
                     (archive.negative_error_domain != archives[0].negative_error_domain ||
                      archive.negative_error_code != archives[0].negative_error_code))) {
                    return false;
                }
                const auto* receipt_image = sealed_shader_cache_detail::find_artifact_image(
                    seed_image->artifacts(),
                    sealed_shader_cache_artifact_kind::metal_binary_archive_qualification,
                    image->descriptor.key_index,
                    image->descriptor.filename + ".qualification");
                if (receipt_image == nullptr)
                    return false;
                const auto receipt =
                    sealed_shader_cache_detail::parse_metal_archive_qualification_receipt(
                        receipt_image->bytes);
                if (!receipt || !receipt->is_production_accepted() ||
                    receipt->archive_size != archive.size ||
                    receipt->archive_sha256 != archive.sha256 ||
                    receipt->negative_error.domain != archive.negative_error_domain ||
                    receipt->negative_error.code != archive.negative_error_code ||
                    receipt->wire_revision != qualification_wire_revision ||
                    receipt->helper_wire_revision != helper_wire_revision ||
                    receipt->helper_build_identity != helper_build_identity ||
                    receipt->helper_source != helper_source ||
                    receipt->helper_executable_size != helper_executable_size ||
                    receipt->helper_executable_sha256 != helper_executable_sha256 ||
                    receipt->helper_metal_registry_id != helper_metal_registry_id) {
                    return false;
                }
            }
        } catch (...) {
            return false;
        }
        return true;
    }

    /// @brief Compares every configuration field.
    [[nodiscard]] bool
    operator==(const runtime_metal_archive_configuration&) const noexcept = default;
};

/// @brief Owns the installed archive settings and their memory reservation.
/// Construction moves the  prepared strings and vectors into this object.
class runtime_metal_archive_configuration_image {
  public:
    /// @brief Keeps configuration ownership and its reservation from being copied.
    runtime_metal_archive_configuration_image(const runtime_metal_archive_configuration_image&) =
        delete;

    /// @brief Prevents copying configuration ownership by assignment.
    runtime_metal_archive_configuration_image&
    operator=(const runtime_metal_archive_configuration_image&) = delete;

    /// @brief Moves the configuration together with its memory reservation.
    runtime_metal_archive_configuration_image(
        runtime_metal_archive_configuration_image&&) noexcept = default;

    /// @brief Prevents assignment from separating settings from their memory reservation.
    runtime_metal_archive_configuration_image&
    operator=(runtime_metal_archive_configuration_image&&) noexcept = delete;

    /// @brief Returns a read-only vew of the owned settings.
    [[nodiscard]] const runtime_metal_archive_configuration& configuration() const noexcept {
        return configuration_;
    }

    /// @brief Returns the owned capacity after the memory reservation was adjusted.
    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        return retained_capacity_bytes_;
    }

    /// @brief Returns the largest memory reservation used to build  the settings.
    [[nodiscard]] std::uint64_t peak_admitted_bytes() const noexcept {
        return peak_admitted_bytes_;
    }

    /// @brief Checks the settings and verifies that their capacity matches the, memory reservation.
    [[nodiscard]] bool is_valid() const noexcept {
        return configuration_.is_valid() && permit_ && retained_capacity_bytes_ != 0u &&
               retained_capacity_bytes_ <= peak_admitted_bytes_ &&
               permit_.bytes() == retained_capacity_bytes_;
    }

  private:
    /// @brief Takes ownership of validated settings and their memory reservation
    /// @param[in] configuration Settings to move into this object.
    /// @param[in] peak_admitted_bytes Reservation made before constructing the settings.
    /// @param[in] permit Reservation covering the owned storage.
    runtime_metal_archive_configuration_image(runtime_metal_archive_configuration configuration,
                                              std::uint64_t peak_admitted_bytes,
                                              bioseq::parallelism::memory_permit permit) noexcept
        : permit_(std::move(permit)), configuration_(std::move(configuration)),
          peak_admitted_bytes_(peak_admitted_bytes) {}

    /// @brief Counts this object and its owned string and vector capacities.
    /// Returns the largest uint64_t value if the sum overflows.
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t owned = sizeof(runtime_metal_archive_configuration_image);
        owned = saturating_add(
            owned,
            saturating_mul(static_cast<std::uint64_t>(configuration_.archives.capacity()),
                           sizeof(runtime_metal_archive_spec)));
        for (const runtime_metal_archive_spec& archive : configuration_.archives) {
            owned = saturating_add(
                owned, static_cast<std::uint64_t>(archive.artifact_filename.capacity()));
            owned = saturating_add(
                owned, static_cast<std::uint64_t>(archive.negative_error_domain.capacity()));
        }
        owned = saturating_add(
            owned,
            static_cast<std::uint64_t>(configuration_.command_library.source.capacity()));
        owned = saturating_add(
            owned,
            static_cast<std::uint64_t>(configuration_.command_library.metallib.capacity()));
        owned = saturating_add(
            owned,
            static_cast<std::uint64_t>(configuration_.command_library.qualification
                                           .helper_build_identity.capacity()));
        owned = saturating_add(
            owned,
            static_cast<std::uint64_t>(
                configuration_.command_library.qualification.helper_source.capacity()));
        owned = saturating_add(
            owned, static_cast<std::uint64_t>(configuration_.helper_build_identity.capacity()));
        owned = saturating_add(owned,
                               static_cast<std::uint64_t>(configuration_.helper_source.capacity()));
        return owned;
    }

    /// @brief Reduces the memory reservation to the capacity this object keeps.
    /// Returns false if the capacity overflows or exceeds the reservation.
    [[nodiscard]] bool reconcile() noexcept {
        const std::uint64_t retained = owned_capacity_bytes();
        if (retained == (std::numeric_limits<std::uint64_t>::max)() || !permit_ ||
            retained > permit_.bytes()) {
            return false;
        }
        permit_.shrink_to(retained);
        retained_capacity_bytes_ = retained;
        return true;
    }

    // Members are destroyed in reverse order, so settings are freed  before the reservation.
    bioseq::parallelism::memory_permit permit_;              ///< Memory reservation for the owned settings.
    runtime_metal_archive_configuration configuration_;     ///< Installed archive settings.
    std::uint64_t peak_admitted_bytes_ = 0u;                ///< Memory reserved before building the settings.
    std::uint64_t retained_capacity_bytes_ = 0u;            ///< Owned capacity covered by the current reservation.

    friend std::expected<void, std::string> install_runtime_metal_archive_configuration(
        std::shared_ptr<const runtime_metal_archive_configuration_image>);
    friend std::expected<std::shared_ptr<const runtime_metal_archive_configuration_image>,
                         std::string>
    make_runtime_metal_archive_configuration_image(
        std::shared_ptr<const sealed_shader_cache_seed_image>,
        std::array<std::uint8_t, VK_UUID_SIZE>,
        std::uint64_t);
};

/// @brief Counters for archive installation and checks of the installed Metal settings.
struct runtime_metal_archive_live_gate_result {
    std::uint64_t install_validation_count = 0u;        ///< Installation attempts that passed configuration validation.
    std::uint64_t live_gate_calls = 0u;                 ///< Calls checking that the installed Metal settings remain unchanged.
    std::uint64_t live_configuration_scan_count = 0u;   ///< Configuration rescans during those checks.
    bool installed_accepted = false;                    ///< Whether installation was accepted.
};

/// @brief Results of testing a temporary archive fqle against its expected bytes.
struct runtime_private_archive_snapshot_test_result {
    std::filesystem::path snapshot_path;       ///< Temporary path used by the  test. Cleanup attempts to remove it on exit.
    std::uint64_t streamed_bytes = 0u;         ///< Bytes read while verifying the temporary file.
    std::uint64_t scratch_capacity_bytes = 0u; ///< Scratch space used to verify the file.
    bool exact_seed_compared = false;          ///< Whether verification compared the file with the supplied seed bytes.
    bool tamper_rejected = false;              ///< Whether a tampered snapshot was rejected.
};

/// @brief Returns archive-installation and live-check counters with the acceptance flag.
[[nodiscard]] runtime_metal_archive_live_gate_result
capture_runtime_metal_archive_live_gate_result() noexcept;

/// @brief Runs the normal Metal settings check with a test operation label.
void probe_immutable_metal_policy_live_for_test();

/// @brief Tests that changing a temporary archive file makes verification fail
/// @param[in] artifact_filename Filename recorded for the test archive.
/// @param[in] bytes Expected archive contents.
/// @param[in] temp_parent Directory  where the temporary snapshot is created.
/// @return Verification results, or a platform or file-operation error.
[[nodiscard]] std::expected<runtime_private_archive_snapshot_test_result, std::string>
verify_runtime_private_archive_snapshot_for_test(std::string_view artifact_filename,
                                                 std::span<const std::byte> bytes,
                                                 const std::filesystem::path& temp_parent);

/// @brief Count, total size, and checksum identifying an ordered archive set.
struct runtime_metal_archive_set_identity {
    std::uint64_t archive_count = 0u;   ///< Number of archives in the ordered set.
    std::uint64_t total_size = 0u;      ///< Total archive bytes, capped if the, sum overflows.
    std::uint64_t ordered_checksum = 0u; ///< Stable FNV checksum over ordered archive fields.

    /// @brief Checks that the archive count, total size, and checksum are nonzero.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return archive_count != 0u && total_size != 0u && ordered_checksum != 0u;
    }

    /// @brief Compares the archive count, total size, and ordered checksum.
    [[nodiscard]] constexpr bool
    operator==(const runtime_metal_archive_set_identity&) const noexcept = default;
};

/// @brief Device and driver details used to check Vulkan pipeline compatibility.
struct runtime_vulkan_device_identity {
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};         ///< Vulkan device UUID bytes.
    std::uint32_t api_version = 0u;                               ///< Packed Vulkan API version.
    std::uint32_t driver_version = 0u;                            ///< Driver version reported by Vulkan.
    std::uint32_t vendor_id = 0u;                                 ///< Vendor idenzifier reported by Vulkan.
    std::uint32_t device_id = 0u;                                 ///< Device  identifier reported by Vulkan.
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{}; ///< Pipeline-cache compatibility UUID.

    /// @brief Checks that API and driver versions and both UUIDs are nonzero.
    [[nodiscard]] bool is_valid() const noexcept {
        return api_version != 0u && driver_version != 0u &&
               std::any_of(device_uuid.begin(),
                           device_uuid.end(),
                           [](std::uint8_t value) { return value != 0u; }) &&
               std::any_of(pipeline_cache_uuid.begin(),
                           pipeline_cache_uuid.end(),
                           [](std::uint8_t value) { return value != 0u; });
    }

    /// @brief Compares all device and driver details.
    [[nodiscard]] bool operator==(const runtime_vulkan_device_identity&) const noexcept = default;
};

/// @brief Record of checks performed when Metal archives were loaded for this process.
struct runtime_metal_archive_process_qualification;
/// @brief Computes a nonzero identifier from the archive-load check record.
[[nodiscard]] std::uint64_t runtime_metal_archive_process_qualification_identity(
    const runtime_metal_archive_process_qualification& qualification) noexcept;

/// @brief Records the archive, device, and seed checks made during Metal archive loading.
/// Later pipeline creation uses this saved record. The archive files have
/// already been checked and removed.
struct runtime_metal_archive_process_qualification {
    std::uint32_t wire_revision = 0u;                    ///< Archive-check record  format version.
    std::uint64_t identity = 0u;                         ///< Hash identifying this check record.
    runtime_metal_archive_set_identity archive_set{};    ///< Ordered archive set that was checked.
    std::uint64_t receipt_count = 0u;                    ///< Number of accepted archive-check records.
    std::uint64_t hash_passes = 0u;                      ///< Complete passes hashing archive bytes during loading.
    std::uint64_t hashed_bytes = 0u;                     ///< Archive bytes hashed during loading.
    std::uint64_t private_files_created = 0u;            ///< Temporary files created to load Metal archives.
    std::uint64_t private_files_unlinked = 0u;           ///< Temporary archive files removed after loading
    std::uint64_t live_private_paths = 0u;               ///< Temporary archive paths still present after loading.
    std::uint64_t pipeline_filesystem_hash_passes = 0u;  ///< Filesystem hash passes during pipeline creation.
    std::uint64_t pipeline_filesystem_hashed_bytes = 0u; ///< Filesystem bytes hashed during pipeline creation.
    std::uint32_t helper_wire_revision = 0u;             ///< Archive-builder record format  version.
    std::string_view helper_build_identity;              ///< Archive-builder build identity label.
    std::string_view helper_source;                      ///< Location cltegory of the archive-builder executable.
    std::uint64_t helper_executable_size = 0u;           ///< Helper executable size in bytes.
    std::array<std::uint8_t, 32> helper_executable_sha256{}; ///< Helper executable SHA-256 digest.
    std::uint64_t helper_metal_registry_id = 0u;         ///< Metal device registry ID used by the archive builder.
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid{}; ///< Vulkan device UUID recorded when archives were loaded.
    std::uint32_t api_version = 0u;                      ///< Vulkan API version recorded during archive loading.
    std::uint32_t driver_version = 0u;                   ///< Driver version recorded during archive loading.
    std::uint32_t vendor_id = 0u;                        ///< Vulkan vendor identifier recorded during archive loading.
    std::uint32_t device_id = 0u;                        ///< Vulkan device, identifier recorded during archive loading.
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{}; ///< Pipeline-cache UUID recorded during archive loading.
    std::uint64_t seed_receipt_identity = 0u;            ///< Identifier  of the accepted seed-load record.
    std::array<std::uint8_t, 32> seed_sha256{};          ///< Seed image SHA-256 digest.
    std::uint64_t seed_size = 0u;                        ///< Seed size in bytes.
    std::uint64_t manifest_checksum = 0u;                ///< Seed manifest checksum.
    bool saturated = true;                                 ///< Whether memory accounting overflowed during archive loading.

    /// @brief Checks that archives were loaded from the accepted seed and temporary files were removed.
    /// Verifies the saved device and builder details, byte counts, and record hash.
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return wire_revision == 2u && identity != 0u &&
               identity == runtime_metal_archive_process_qualification_identity(*this) &&
               archive_set.is_valid() && receipt_count == archive_set.archive_count &&
               hash_passes == 1u && hashed_bytes == archive_set.total_size &&
               private_files_created == archive_set.archive_count &&
               private_files_unlinked == private_files_created && live_private_paths == 0u &&
               pipeline_filesystem_hash_passes == 0u && pipeline_filesystem_hashed_bytes == 0u &&
               helper_wire_revision == 2u &&
               helper_build_identity == "bioseq_gpu_archive_builder" &&
               helper_source == "installed-sibling" && helper_executable_size != 0u &&
               std::any_of(helper_executable_sha256.begin(),
                           helper_executable_sha256.end(),
                           [](std::uint8_t value) { return value != 0u; }) &&
               helper_metal_registry_id != 0u &&
               runtime_vulkan_device_identity{
                   .device_uuid = vulkan_device_uuid,
                   .api_version = api_version,
                   .driver_version = driver_version,
                   .vendor_id = vendor_id,
                   .device_id = device_id,
                   .pipeline_cache_uuid = pipeline_cache_uuid,
               }
                   .is_valid() &&
               seed_receipt_identity != 0u && seed_size != 0u && manifest_checksum != 0u &&
               std::any_of(seed_sha256.begin(),
                           seed_sha256.end(),
                           [](std::uint8_t value) { return value != 0u; }) &&
               !saturated;
    }

    /// @brief Compares every field in the archive-load check record.
    [[nodiscard]] bool
    operator==(const runtime_metal_archive_process_qualification&) const noexcept = default;
};

/// @brief Hashes the archive-load check fields in a fixed order.
/// The identity field itself is excluded.
/// @param[in] value Check record to identify.
/// @return A nonzero FNV-based  identifier.
[[nodiscard]] inline std::uint64_t runtime_metal_archive_process_qualification_identity(
    const runtime_metal_archive_process_qualification& value) noexcept {
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    std::uint64_t identity = fnv_offset;
    const auto append_byte = [&identity](std::uint8_t byte) {
        // Add one byte to the FNV hash.
        identity ^= byte;
        identity *= fnv_prime;
    };
    const auto append_u64 = [&append_byte](std::uint64_t number) {
        // Hash integers in little-endian byte order.
        for (std::uint32_t shift = 0u; shift != 64u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(number >> shift));
        }
    };
    const auto append_text = [&append_byte](std::string_view text) {
        // End each text fied with a zero byte to keep adjacent fields distinct
        for (const unsigned char byte : text)
            append_byte(byte);
        append_byte(0u);
    };
    append_u64(value.wire_revision);
    append_u64(value.archive_set.archive_count);
    append_u64(value.archive_set.total_size);
    append_u64(value.archive_set.ordered_checksum);
    append_u64(value.receipt_count);
    append_u64(value.hash_passes);
    append_u64(value.hashed_bytes);
    append_u64(value.private_files_created);
    append_u64(value.private_files_unlinked);
    append_u64(value.live_private_paths);
    append_u64(value.pipeline_filesystem_hash_passes);
    append_u64(value.pipeline_filesystem_hashed_bytes);
    append_u64(value.helper_wire_revision);
    append_text(value.helper_build_identity);
    append_text(value.helper_source);
    append_u64(value.helper_executable_size);
    for (const std::uint8_t byte : value.helper_executable_sha256) {
        append_byte(byte);
    }
    append_u64(value.helper_metal_registry_id);
    for (const std::uint8_t byte : value.vulkan_device_uuid)
        append_byte(byte);
    append_u64(value.api_version);
    append_u64(value.driver_version);
    append_u64(value.vendor_id);
    append_u64(value.device_id);
    for (const std::uint8_t byte : value.pipeline_cache_uuid)
        append_byte(byte);
    append_u64(value.seed_receipt_identity);
    for (const std::uint8_t byte : value.seed_sha256)
        append_byte(byte);
    append_u64(value.seed_size);
    append_u64(value.manifest_checksum);
    append_u64(value.saturated ? 1u : 0u);
    return identity == 0u ? fnv_offset : identity;
}

/// @brief Computes an archive-set checksum and total size in the given order.
/// @param[in] archives Archive details to include.
/// @return Archive count, total bytes, and checksum. The byte total is capped on overflow.
[[nodiscard]] inline runtime_metal_archive_set_identity make_runtime_metal_archive_set_identity(
    std::span<const runtime_metal_archive_spec> archives) noexcept {
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    std::uint64_t checksum = fnv_offset;
    std::uint64_t total_size = 0u;
    const auto append_byte = [&checksum](const std::uint8_t byte) {
        // Add the next archive-detail byte to the ordered checksum.
        checksum ^= byte;
        checksum *= fnv_prime;
    };
    for (const runtime_metal_archive_spec& archive : archives) {
        for (std::uint32_t shift = 0u; shift != 32u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(archive.artifact_ordinal >> shift));
        }
        for (const unsigned char byte : archive.artifact_filename) {
            append_byte(byte);
        }
        append_byte(0u);
        for (std::uint32_t shift = 0u; shift != 64u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(archive.size >> shift));
        }
        for (const std::uint8_t byte : archive.sha256)
            append_byte(byte);
        for (const unsigned char byte : archive.negative_error_domain) {
            append_byte(byte);
        }
        append_byte(0u);
        const std::uint64_t error_code_bits =
            std::bit_cast<std::uint64_t>(archive.negative_error_code);
        for (std::uint32_t shift = 0u; shift != 64u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(error_code_bits >> shift));
        }
        if (total_size > (std::numeric_limits<std::uint64_t>::max)() - archive.size) {
            total_size = (std::numeric_limits<std::uint64_t>::max)();
        } else {
            total_size += archive.size;
        }
    }
    return runtime_metal_archive_set_identity{
        .archive_count = static_cast<std::uint64_t>(archives.size()),
        .total_size = total_size,
        .ordered_checksum = checksum,
    };
}

/// @brief Records the checks made while creating one Vulkan compute pipeline.
/// On Metal, creation first tries the same descriptor with an empty archive
/// and requires the expected miss. It then uses the  accepted archives with
/// compilation on a cache miss disabled. Linux records native Vulkan creation.
struct runtime_metal_archive_pipeline_audit {
    // Linux records the shader module and pipeline handles from successful
    // Vulkan creation. Metal archive checks apply on macOS.
    bool native_vulkan = false;
    std::uint64_t native_shader_module_identity = 0u;
    std::uint64_t native_pipeline_identity = 0u;
    std::uint64_t receipt_identity = 0u;                 ///< Identifier assigned to this pipeline-creation record.
    std::uint64_t scope_identity = 0u;                   ///< Identifier of the pipeline-creation scope.
    std::string cache_digest;                            ///< Cache digest from the accepted seed manifest.
    std::array<std::uint8_t, 32> seed_sha256{};          ///< Seed image SHA-256 digest.
    std::uint64_t seed_size = 0u;                        ///< Loaded seed, size in bytes.
    std::uint64_t manifest_checksum = 0u;                ///< Seed manifest checksum.
    std::uint64_t seed_receipt_identity = 0u;            ///< Identifier of the  apcepted seed-load record.
    std::uint64_t seed_load_count = 0u;                  ///< Loads recorded when the process accepted the seed.
    std::uint64_t seed_manifest_read_count = 0u;         ///< Complete manifest reads recorded for the loaded seed image.
    std::uint64_t seed_artifact_read_count = 0u;         ///< Files read into the loaded seed image.
    std::uint64_t seed_manifest_bytes = 0u;              ///< Byte size of the saved seed manifest.
    std::uint64_t seed_artifact_bytes = 0u;              ///< Combined bytes of loaded seed files, excluding the manifest.
    std::uint64_t seed_load_ns = 0u;                     ///< Seed load duration in nanoseconds.
    std::uint64_t seed_admission_count = 0u;             ///< Memory reservations made while loading the seed.
    std::uint64_t seed_peak_admitted_bytes = 0u;         ///< Largest memory reservation during seed loading
    std::uint64_t seed_retained_capacity_bytes = 0u;     ///< Seed capacity kept after its memory reservation was  reduced.
    bool seed_admission_saturated = true;                  ///< Whether seed-load memory accounting overflowed.
    runtime_metal_archive_process_qualification cache_qualification{}; ///< Saved checks from loading the process's Metal archives.
    shader_variant_key shader_key{};                     ///< Shader variant used to create this pipeline.
    std::string expected_entry_name;                     ///< Expected shader entry-point name.
    std::string observed_entry_name;                     ///< Shader entry-point name observed during creation.
    std::uint64_t matched_descriptor_functions = 0u;     ///< Descriptor functions matching the requested shader.
    runtime_metal_archive_set_identity expected_archives{}; ///< Archive set required by the installed settings.
    runtime_metal_archive_set_identity loaded_archives{}; ///< Archive set already loaded in the process.
    std::uint64_t archive_pre_post_identity_checks = 0u; ///< Archive file checks completed when the process loaded the archives.
    std::uint64_t pipeline_pre_identity_checks = 0u;     ///< Extra archive-file checks before pipeline creation. Must  stay zero.
    std::uint64_t pipeline_post_identity_checks = 0u;    ///< Extra archive-file checks after pipeline creation. Must stay zero.
    std::uint64_t unsealed_metal_source_requests = 0u;   ///< Requests to compile Metal souce outside the accepted command library.
    std::uint64_t metal_source_fallback_calls = 0u;      ///< Fallback Metal source compilations observed.
    bool command_interpose_installed = false;            ///< Whether the command-library loading wrappers are installed.
    bool command_interpose_immutable_fail_closed = false; ///< Whether changed command-library settings cause a failure.
    std::uint64_t command_loader_metal_registry_id = 0u; ///< Metal device registry ID used by the command-library loader.
    std::uint64_t command_source_methods_present = 0u;   ///< Source-loading methods found on the Metal device.
    std::uint64_t command_source_wrapper_methods = 0u; ///< Source-loading methods replaced by the installed wrappers.
    std::array<std::uint8_t, VK_UUID_SIZE> configured_vulkan_device_uuid{}; ///< Vulkan device UUID selected when  the archives were installed.
    std::array<std::uint8_t, VK_UUID_SIZE> runtime_vulkan_device_uuid{}; ///< Runtime Vulkan device UUID.
    std::uint32_t api_version = 0u;                      ///< Packed Vulkan API version at pipeline time.
    std::uint32_t driver_version = 0u;                   ///< Driver version at pipeline time.
    std::uint32_t vendor_id = 0u;                        ///< Vendor identifier reported by Vulkan for this pipeline.
    std::uint32_t device_id = 0u;                        ///< Device identifier reported by Vulkan for this pipeline.
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{}; ///< Pipeline-cache UUID at pipeline time.
    std::uint64_t pipeline_create_ns = 0u;               ///< vkCreateComputePipelines duration in nanoseconds.
    std::uint64_t expected_metal_registry_id = 0u;       ///< Expected Metal registry identifier.
    std::uint64_t loader_metal_registry_id = 0u;         ///< Metal device registry ID used to load the archives.
    std::uint64_t intercepted_metal_registry_id = 0u;    ///< Metal registry identifier on the intercepted call.
    std::uint64_t scoped_interceptions = 0u;             ///< Metal pipeline calls observed within this creation scope.
    std::uint64_t descriptor_replacements = 0u;          ///< Descriptors changed to use the accepted archive set
    std::uint64_t fail_on_miss_enforcements = 0u;        ///< Calls required to fail if the pipeline is absent from the archives.
    std::uint64_t same_descriptor_negative_controls = 0u; ///< Empty-archive tests run with, the same pipeline descriptor.
    std::uint64_t negative_control_misses = 0u;          ///< Empty-archive tests that returned the expected miss.
    std::string expected_negative_error_domain;          ///< Expected error domain from  an empty archive.
    std::int64_t expected_negative_error_code = 0;       ///< Expected error code from an empty archive.
    std::string observed_negative_error_domain;          ///< Error domain returned by the empty-archive test.
    std::int64_t observed_negative_error_code = 0;       ///< Error code returned by the empty-archive test.
    std::uint64_t original_sync_negative_calls = 0u;     ///< Calls to the original synchronos method for empty-archive tests.
    std::uint64_t actual_sync_calls = 0u;                ///< Synchronous pipeline calls observed in scope.
    std::uint64_t actual_async_calls = 0u;               ///< Asynchronous pipeline calls observed in scope.
    std::uint64_t outside_scope_interceptions = 0u;      ///< Pipeline calls observed outside the active scope during creation.
    bool nested_scope = false;                           ///< Whether a nested interception scope was entered.
    bool sticky_failure = false;                         ///< Whether an earlier failure remains recorded.
    bool outer_vulkan_pipeline_succeeded = false;        ///< Whether the outer Vulkan pipeline creation succeeded.

    /// @brief Checks that pipeline creation succeeded and its saved details agree.
    /// Metal records must  also show an empty-archive miss followed by use of
    /// the accepted archives, with source-compilation fallback disabled.
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        if (native_vulkan) {
            return k_native_vulkan_pipeline_receipts && receipt_identity != 0u &&
                   native_shader_module_identity != 0u && native_pipeline_identity != 0u &&
                   is_shader_variant_key_valid(shader_key) &&
                   expected_entry_name == "main" && observed_entry_name == expected_entry_name &&
                   outer_vulkan_pipeline_succeeded && pipeline_create_ns != 0u &&
                   api_version != 0u && driver_version != 0u &&
                   std::any_of(runtime_vulkan_device_uuid.begin(),
                               runtime_vulkan_device_uuid.end(),
                               [](std::uint8_t byte) { return byte != 0u; }) &&
                   std::any_of(pipeline_cache_uuid.begin(), pipeline_cache_uuid.end(),
                               [](std::uint8_t byte) { return byte != 0u; }) &&
                   scope_identity == 0u && cache_digest.empty() &&
                   seed_sha256 == std::array<std::uint8_t, 32>{} && seed_size == 0u &&
                   manifest_checksum == 0u && seed_receipt_identity == 0u &&
                   seed_load_count == 0u && seed_load_ns == 0u &&
                   seed_manifest_read_count == 0u && seed_artifact_read_count == 0u &&
                   seed_manifest_bytes == 0u && seed_artifact_bytes == 0u &&
                   seed_admission_count == 0u && seed_peak_admitted_bytes == 0u &&
                   seed_retained_capacity_bytes == 0u &&
                   cache_qualification == runtime_metal_archive_process_qualification{} &&
                   expected_archives == runtime_metal_archive_set_identity{} &&
                   loaded_archives == runtime_metal_archive_set_identity{} &&
                   !command_interpose_installed && !command_interpose_immutable_fail_closed &&
                   scoped_interceptions == 0u && outside_scope_interceptions == 0u &&
                   actual_sync_calls == 0u && actual_async_calls == 0u &&
                   !nested_scope && !sticky_failure;
        }
        if (native_shader_module_identity != 0u || native_pipeline_identity != 0u)
            return false;
        const std::uint64_t actual_calls = actual_sync_calls + actual_async_calls;
        return receipt_identity != 0u && scope_identity != 0u &&
               is_runtime_cache_digest_valid(cache_digest) &&
               std::any_of(seed_sha256.begin(),
                           seed_sha256.end(),
                           [](std::uint8_t byte) { return byte != 0u; }) &&
               seed_size != 0u && manifest_checksum != 0u && seed_receipt_identity != 0u &&
               seed_load_count == 1u && seed_manifest_read_count == 1u &&
               seed_artifact_read_count != 0u && seed_manifest_bytes != 0u &&
               seed_artifact_bytes == seed_size && seed_load_ns != 0u &&
               seed_admission_count == 1u && seed_peak_admitted_bytes != 0u &&
               seed_retained_capacity_bytes != 0u &&
               seed_retained_capacity_bytes <= seed_peak_admitted_bytes &&
               !seed_admission_saturated && cache_qualification.is_internally_consistent() &&
               cache_qualification.archive_set == expected_archives &&
               cache_qualification.seed_receipt_identity == seed_receipt_identity &&
               cache_qualification.seed_sha256 == seed_sha256 &&
               cache_qualification.seed_size == seed_size &&
               cache_qualification.manifest_checksum == manifest_checksum &&
               is_shader_variant_key_valid(shader_key) && !expected_entry_name.empty() &&
               observed_entry_name == expected_entry_name && matched_descriptor_functions == 1u &&
               expected_archives.is_valid() && expected_archives == loaded_archives &&
               archive_pre_post_identity_checks == expected_archives.archive_count &&
               pipeline_pre_identity_checks == 0u && pipeline_post_identity_checks == 0u &&
               unsealed_metal_source_requests == 0u && metal_source_fallback_calls == 0u &&
               command_interpose_installed && command_interpose_immutable_fail_closed &&
               configured_vulkan_device_uuid == runtime_vulkan_device_uuid &&
               std::any_of(runtime_vulkan_device_uuid.begin(),
                           runtime_vulkan_device_uuid.end(),
                           [](std::uint8_t byte) { return byte != 0u; }) &&
               api_version != 0u && driver_version != 0u &&
               std::any_of(pipeline_cache_uuid.begin(),
                           pipeline_cache_uuid.end(),
                           [](std::uint8_t byte) { return byte != 0u; }) &&
               pipeline_create_ns != 0u &&
               cache_qualification.helper_metal_registry_id == expected_metal_registry_id &&
               cache_qualification.vulkan_device_uuid == runtime_vulkan_device_uuid &&
               cache_qualification.api_version == api_version &&
               cache_qualification.driver_version == driver_version &&
               cache_qualification.vendor_id == vendor_id &&
               cache_qualification.device_id == device_id &&
               cache_qualification.pipeline_cache_uuid == pipeline_cache_uuid &&
               expected_metal_registry_id != 0u &&
               command_loader_metal_registry_id == expected_metal_registry_id &&
               command_source_methods_present == 2u &&
               command_source_wrapper_methods == command_source_methods_present &&
               loader_metal_registry_id == expected_metal_registry_id &&
               intercepted_metal_registry_id == expected_metal_registry_id &&
               scoped_interceptions == 1u && actual_calls == scoped_interceptions &&
               descriptor_replacements == scoped_interceptions &&
               fail_on_miss_enforcements == scoped_interceptions &&
               same_descriptor_negative_controls == scoped_interceptions &&
               negative_control_misses == scoped_interceptions &&
               !expected_negative_error_domain.empty() &&
               observed_negative_error_domain == expected_negative_error_domain &&
               observed_negative_error_code == expected_negative_error_code &&
               original_sync_negative_calls == scoped_interceptions &&
               outside_scope_interceptions == 0u && !nested_scope && !sticky_failure &&
               outer_vulkan_pipeline_succeeded;
    }
};

namespace runtime_detail {
/// @brief Checks cache metrics before marking their records as counted.
class runtime_gpu_cache_metrics_claim_plan;
} // namespace runtime_detail

/// @brief Keeps the accepted creation record with its cached Vulkan pipeline.
/// Cache users share this object. Moving transfers its record and the flag
/// that prevents cache metrics from being counted twice.
class runtime_metal_archive_pipeline_receipt {
  public:
    /// @brief Creates an empty invalid receipt.
    runtime_metal_archive_pipeline_receipt() = default;

    /// @brief Prevents copying a record whose metrics can be counted only once.
    runtime_metal_archive_pipeline_receipt(const runtime_metal_archive_pipeline_receipt&) = delete;

    /// @brief Prevents copying the record and its metrics state by assignment.
    runtime_metal_archive_pipeline_receipt&
    operator=(const runtime_metal_archive_pipeline_receipt&) = delete;

    /// @brief Moves the creation  record and its metrics state, leaving the source empty
    runtime_metal_archive_pipeline_receipt(runtime_metal_archive_pipeline_receipt&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          metrics_claimed_(std::exchange(other.metrics_claimed_, false)) {}

    /// @brief Replaces this record by moving another record and its metrics state.
    runtime_metal_archive_pipeline_receipt&
    operator=(runtime_metal_archive_pipeline_receipt&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            metrics_claimed_ = std::exchange(other.metrics_claimed_, false);
        }
        return *this;
    }

    /// @brief Checks that the saved pipeline-creation details are consistent.
    [[nodiscard]] bool is_valid() const noexcept {
        return audit_.is_internally_consistent();
    }

    /// @brief Returns a read-only view of the savzd pipeline-creation details.
    [[nodiscard]] const runtime_metal_archive_pipeline_audit& audit() const noexcept {
        return audit_;
    }

    /// @brief Counts this object and its owned string capacities.
    /// Returns the largest uint64_t value if the total overflows.
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::common::saturating_add;
        std::uint64_t owned = sizeof(runtime_metal_archive_pipeline_receipt);
        owned = saturating_add(owned, audit_.cache_digest.capacity());
        owned = saturating_add(owned, audit_.expected_entry_name.capacity());
        owned = saturating_add(owned, audit_.observed_entry_name.capacity());
        owned = saturating_add(owned, audit_.expected_negative_error_domain.capacity());
        owned = saturating_add(owned, audit_.observed_negative_error_domain.capacity());
        return owned;
    }

    /// @brief Tests that moving invalidates the source and cache views share one record.
    /// @param[in] audit Valid  pipeline-creation details for the test.
    /// @return Whether move and shared-pointer checks pass.
    [[nodiscard]] static bool
    self_test_move_and_reuse_contract(runtime_metal_archive_pipeline_audit audit) {
        runtime_metal_archive_pipeline_receipt issued{std::move(audit)};
        if (!issued.is_valid())
            return false;
        runtime_metal_archive_pipeline_receipt moved{std::move(issued)};
        if (issued.is_valid() || !moved.is_valid())
            return false;
        auto retained =
            std::make_shared<const runtime_metal_archive_pipeline_receipt>(std::move(moved));
        const auto normal_cache_view = retained;
        const auto compute_cache_view = retained;
        return retained->is_valid() && normal_cache_view->is_valid() &&
               compute_cache_view->is_valid() && retained.get() == normal_cache_view.get() &&
               retained.get() == compute_cache_view.get();
    }

  private:
    /// @brief Takes ownership, of a completed pipeline-creation record.
    explicit runtime_metal_archive_pipeline_receipt(
        runtime_metal_archive_pipeline_audit audit) noexcept
        : audit_(std::move(audit)) {}

    runtime_metal_archive_pipeline_audit audit_{}; ///< Saved pipeline-creation details.
    mutable bool metrics_claimed_ = false;         ///< Whether this record's cache metrics have already been counted.
    friend class runtime;
    friend class runtime_pipeline_operation_token;
    friend class runtime_detail::runtime_gpu_cache_metrics_claim_plan;
};

static_assert(!std::is_copy_constructible_v<runtime_metal_archive_pipeline_receipt>);
static_assert(std::is_nothrow_move_constructible_v<runtime_metal_archive_pipeline_receipt>);

/// @brief Installs archive settings before the supervisor starts the runtime.
/// Reinstalling identical settings succeeds. Different settings are rejected
/// after the first successful installation.
/// @param[in] configuration Valid settings with a memory  reservation.
/// @return Success, or an error describing invalid or conflicting settings.
[[nodiscard]] std::expected<void, std::string> install_runtime_metal_archive_configuration(
    std::shared_ptr<const runtime_metal_archive_configuration_image> configuration);

/// @brief Prepares runtime archive settings from an accepted shader-cache seed.
/// Reserves memory before building the settings and checks every archive
/// against its saved builder record.
/// @param[in] seed_image Accepted seed kept in memory.
/// @param[in] vulkan_device_uuid Optional Vulkan device UUID to require.
/// @param[in] metal_device_registry_id Optional Metal device registry ID to require.
/// @return Shared settings, or a validation or memory-rezervation error.
[[nodiscard]] std::expected<std::shared_ptr<const runtime_metal_archive_configuration_image>,
                            std::string>
make_runtime_metal_archive_configuration_image(
    std::shared_ptr<const sealed_shader_cache_seed_image> seed_image,
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid = {},
    std::uint64_t metal_device_registry_id = 0u);

/// @brief Returns whether archive settings have been  successfully installed.
[[nodiscard]] bool is_runtime_metal_archive_configuration_installed() noexcept;

} // namespace bioseq::gpu::vulkan

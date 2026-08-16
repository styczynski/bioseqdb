/**
 * @file metal_archive.cppm
 * @brief Defines immutable Metal archive configuration, identity, and receipts.
 */
module;

#include <cstddef>
#include <cstdint>
#include <libassert/assert.hpp>

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

/// @brief One exact Metal binary archive inside the process-owned sealed seed image.
/// The ordinal binds runtime staging to retained bytes rather than a cache
/// pathname that could be replaced after supervisor validation.
struct runtime_metal_archive_spec {
    std::uint32_t artifact_ordinal = (std::numeric_limits<std::uint32_t>::max)();
    std::string artifact_filename;
    std::uint64_t size = 0u;
    std::array<std::uint8_t, 32> sha256{};
    std::string negative_error_domain;
    std::int64_t negative_error_code = 0;

    /// @brief Checks the ordinal, filename, digest, and negative-control identity.
    /// @return Whether this specification can identify one sealed archive artifact.
    [[nodiscard]] bool is_valid() const noexcept {
        return artifact_ordinal != (std::numeric_limits<std::uint32_t>::max)() &&
               !artifact_filename.empty() && size != 0u && !negative_error_domain.empty() &&
               negative_error_domain.size() <= 1024u &&
               negative_error_domain.find('\0') == std::string::npos &&
               std::any_of(
                   sha256.begin(), sha256.end(), [](std::uint8_t byte) { return byte != 0u; });
    }

    /// @brief Compares every persisted archive identity field.
    [[nodiscard]] bool operator==(const runtime_metal_archive_spec&) const noexcept = default;
};

/// @brief Immutable input installed by the PostgreSQL cache owner before runtime is
/// constructed.  It intentionally contains no directory: an unlisted archive
/// can never become eligible merely by appearing beside a listed one.
struct runtime_metal_archive_configuration {
    std::shared_ptr<const sealed_shader_cache_seed_image> seed_image;
    /// @brief Optional external expectations.  Zero means runtime must seal the
    /// selected Vulkan/Metal identities before vkCreateDevice. A nonzero value
    /// is an additional equality constraint, never a value to trust blindly.
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid{};
    std::uint64_t metal_device_registry_id = 0u;
    std::uint32_t qualification_wire_revision = 0u;
    std::uint64_t qualification_receipt_count = 0u;
    std::uint32_t helper_wire_revision = 0u;
    std::string helper_build_identity;
    std::string helper_source;
    std::uint64_t helper_executable_size = 0u;
    std::array<std::uint8_t, 32> helper_executable_sha256{};
    std::uint64_t helper_metal_registry_id = 0u;
    std::vector<runtime_metal_archive_spec> archives;

    /// @brief Validates the accepted seed, helper identity, ordered artifacts, and receipts.
    /// @return Whether the complete installed archive configuration is self-consistent.
    [[nodiscard]] bool is_valid() const noexcept {
        if (!seed_image || !seed_image->is_accepted() || archives.empty() ||
            qualification_wire_revision != 2u || qualification_receipt_count != archives.size() ||
            helper_wire_revision != 2u || helper_build_identity != "bioseq_gpu_archive_builder" ||
            helper_source != "installed-sibling" || helper_executable_size == 0u ||
            helper_metal_registry_id == 0u ||
            std::all_of(helper_executable_sha256.begin(),
                        helper_executable_sha256.end(),
                        [](std::uint8_t value) { return value == 0u; }) ||
            (metal_device_registry_id != 0u &&
             metal_device_registry_id != helper_metal_registry_id) ||
            !is_runtime_cache_digest_valid(seed_image->manifest().cache_digest)) {
            return false;
        }
        try {
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

    /// @brief Compares every installed archive-configuration field.
    [[nodiscard]] bool
    operator==(const runtime_metal_archive_configuration&) const noexcept = default;
};

/// @brief Single admitted process owner for installed archive configuration metadata.
/// The public configuration is a transient installation candidate. Its vector
/// and strings are moved into this image without a second allocation or copy.
class runtime_metal_archive_configuration_image {
  public:
    /// @brief Prevents copying an admitted archive-configuration owner.
    runtime_metal_archive_configuration_image(const runtime_metal_archive_configuration_image&) =
        delete;

    /// @brief Prevents copy assignment of admitted configuration ownership.
    runtime_metal_archive_configuration_image&
    operator=(const runtime_metal_archive_configuration_image&) = delete;

    /// @brief Transfers the admitted immutable configuration image.
    runtime_metal_archive_configuration_image(
        runtime_metal_archive_configuration_image&&) noexcept = default;

    /// @brief Prevents replacing the lifetime-bound admission permit by assignment.
    runtime_metal_archive_configuration_image&
    operator=(runtime_metal_archive_configuration_image&&) noexcept = delete;

    /// @brief Returns the immutable installed configuration.
    /// @return Borrowed configuration view valid for this image's lifetime.
    [[nodiscard]] const runtime_metal_archive_configuration& configuration() const noexcept {
        return configuration_;
    }

    /// @brief Returns the memory still owned after capacity reconciliation.
    /// @return Retained capacity in bytes.
    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        return retained_capacity_bytes_;
    }

    /// @brief Returns the peak admission acquired while constructing the image.
    /// @return Peak admitted byte count.
    [[nodiscard]] std::uint64_t peak_admitted_bytes() const noexcept {
        return peak_admitted_bytes_;
    }

    /// @brief Checks configuration validity and exact retained-permit accounting.
    /// @return Whether configuration identity and admission state are consistent.
    [[nodiscard]] bool is_valid() const noexcept {
        return configuration_.is_valid() && permit_ && retained_capacity_bytes_ != 0u &&
               retained_capacity_bytes_ <= peak_admitted_bytes_ &&
               permit_.bytes() == retained_capacity_bytes_;
    }

  private:
    /// @brief Adopts validated configuration storage and its admission permit.
    /// @param[in] configuration Configuration whose owned storage is transferred.
    /// @param[in] peak_admitted_bytes Original peak admission size.
    /// @param[in] permit Admission permit covering the transferred storage.
    runtime_metal_archive_configuration_image(runtime_metal_archive_configuration configuration,
                                              std::uint64_t peak_admitted_bytes,
                                              bioseq::parallelism::memory_permit permit) noexcept
        : permit_(std::move(permit)), configuration_(std::move(configuration)),
          peak_admitted_bytes_(peak_admitted_bytes) {}

    /// @brief Measures every capacity owned directly by the immutable image.
    /// @return Saturating owned-capacity total in bytes.
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
            owned, static_cast<std::uint64_t>(configuration_.helper_build_identity.capacity()));
        owned = saturating_add(owned,
                               static_cast<std::uint64_t>(configuration_.helper_source.capacity()));
        return owned;
    }

    /// @brief Shrinks the admission permit to the retained owned capacity.
    /// @return Whether retained capacity is finite and covered by the permit.
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

    // Reverse destruction releases strings/vector before their permit.
    bioseq::parallelism::memory_permit permit_;
    runtime_metal_archive_configuration configuration_;
    std::uint64_t peak_admitted_bytes_ = 0u;
    std::uint64_t retained_capacity_bytes_ = 0u;

    friend std::expected<void, std::string> install_runtime_metal_archive_configuration(
        std::shared_ptr<const runtime_metal_archive_configuration_image>);
    friend std::expected<std::shared_ptr<const runtime_metal_archive_configuration_image>,
                         std::string>
    make_runtime_metal_archive_configuration_image(
        std::shared_ptr<const sealed_shader_cache_seed_image>,
        std::array<std::uint8_t, VK_UUID_SIZE>,
        std::uint64_t);
};

/// @brief Counters and acceptance state for the immutable Metal archive live gate.
struct runtime_metal_archive_live_gate_result {
    std::uint64_t install_validation_count = 0u;
    std::uint64_t live_gate_calls = 0u;
    std::uint64_t live_configuration_scan_count = 0u;
    bool installed_accepted = false;
};

/// @brief Test-only report for private archive snapshot validation.
struct runtime_private_archive_snapshot_test_result {
    std::filesystem::path snapshot_path;
    std::uint64_t streamed_bytes = 0u;
    std::uint64_t scratch_capacity_bytes = 0u;
    bool exact_seed_compared = false;
    bool tamper_rejected = false;
};

/// @brief Captures the current immutable Metal archive live-gate counters.
/// @return Current live-gate counters and acceptance state.
[[nodiscard]] runtime_metal_archive_live_gate_result
capture_runtime_metal_archive_live_gate_result() noexcept;

/// @brief Executes the immutable Metal policy live gate in test builds.
void probe_immutable_metal_policy_live_for_test();

/// @brief Validates a private archive snapshot against supplied immutable bytes.
/// @param[in] artifact_filename Stable archive artifact filename.
/// @param[in] bytes Immutable expected archive bytes.
/// @param[in] temp_parent Parent directory for the private validation snapshot.
/// @return Snapshot validation counters or a platform or I/O diagnostic.
[[nodiscard]] std::expected<runtime_private_archive_snapshot_test_result, std::string>
verify_runtime_private_archive_snapshot_for_test(std::string_view artifact_filename,
                                                 std::span<const std::byte> bytes,
                                                 const std::filesystem::path& temp_parent);

/// @brief Stable aggregate identity of the exact ordered Metal archive set.
struct runtime_metal_archive_set_identity {
    std::uint64_t archive_count = 0u;
    std::uint64_t total_size = 0u;
    std::uint64_t ordered_checksum = 0u;

    /// @brief Checks the aggregate archive count, extent, and checksum.
    /// @return Whether all identity components are nonzero.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return archive_count != 0u && total_size != 0u && ordered_checksum != 0u;
    }

    /// @brief Compares every ordered archive-set identity field.
    [[nodiscard]] constexpr bool
    operator==(const runtime_metal_archive_set_identity&) const noexcept = default;
};

/// @brief Exact Vulkan device and pipeline-cache compatibility identity.
struct runtime_vulkan_device_identity {
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};

    /// @brief Checks required Vulkan version and UUID identity fields.
    /// @return Whether both UUIDs and required versions are populated.
    [[nodiscard]] bool is_valid() const noexcept {
        return api_version != 0u && driver_version != 0u &&
               std::any_of(device_uuid.begin(),
                           device_uuid.end(),
                           [](std::uint8_t value) { return value != 0u; }) &&
               std::any_of(pipeline_cache_uuid.begin(),
                           pipeline_cache_uuid.end(),
                           [](std::uint8_t value) { return value != 0u; });
    }

    /// @brief Compares every device and pipeline-cache compatibility field.
    [[nodiscard]] bool operator==(const runtime_vulkan_device_identity&) const noexcept = default;
};

/// @brief Immutable qualification issued for the process-resident Metal archive set.
struct runtime_metal_archive_process_qualification;
/// @brief Computes the stable identity of a process Metal archive qualification.
/// @param[in] qualification Process qualification to identify.
/// @return Stable nonzero qualification identity.
[[nodiscard]] std::uint64_t runtime_metal_archive_process_qualification_identity(
    const runtime_metal_archive_process_qualification& qualification) noexcept;

/// @brief Immutable process qualification created once while exact seed bytes are
/// installed into live Metal archive objects. Pipeline scopes borrow this
/// record. They never reopen or hash an archive pathname.
struct runtime_metal_archive_process_qualification {
    std::uint32_t wire_revision = 0u;
    std::uint64_t identity = 0u;
    runtime_metal_archive_set_identity archive_set{};
    std::uint64_t receipt_count = 0u;
    std::uint64_t hash_passes = 0u;
    std::uint64_t hashed_bytes = 0u;
    std::uint64_t private_files_created = 0u;
    std::uint64_t private_files_unlinked = 0u;
    std::uint64_t live_private_paths = 0u;
    std::uint64_t pipeline_filesystem_hash_passes = 0u;
    std::uint64_t pipeline_filesystem_hashed_bytes = 0u;
    std::uint32_t helper_wire_revision = 0u;
    std::string_view helper_build_identity;
    std::string_view helper_source;
    std::uint64_t helper_executable_size = 0u;
    std::array<std::uint8_t, 32> helper_executable_sha256{};
    std::uint64_t helper_metal_registry_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::uint64_t seed_receipt_identity = 0u;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    bool saturated = true;

    /// @brief Checks the stable identity, archive lifecycle, helper, device, and seed fields.
    /// @return Whether the process qualification is complete and unsaturated.
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

    /// @brief Compares every immutable process-qualification field.
    [[nodiscard]] bool
    operator==(const runtime_metal_archive_process_qualification&) const noexcept = default;
};

/// @brief Hashes every immutable process-qualification field in wire order.
/// @param[in] value Process qualification to identify.
/// @return Stable nonzero FNV-based qualification identity.
[[nodiscard]] inline std::uint64_t runtime_metal_archive_process_qualification_identity(
    const runtime_metal_archive_process_qualification& value) noexcept {
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    std::uint64_t identity = fnv_offset;
    const auto append_byte = [&identity](std::uint8_t byte) {
        identity ^= byte;
        identity *= fnv_prime;
    };
    const auto append_u64 = [&append_byte](std::uint64_t number) {
        for (std::uint32_t shift = 0u; shift != 64u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(number >> shift));
        }
    };
    const auto append_text = [&append_byte](std::string_view text) {
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

/// @brief Hashes an ordered archive specification set and accumulates its extent.
/// @param[in] archives Ordered archive specifications.
/// @return Aggregate count, size, and stable ordered checksum.
[[nodiscard]] inline runtime_metal_archive_set_identity make_runtime_metal_archive_set_identity(
    std::span<const runtime_metal_archive_spec> archives) noexcept {
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    std::uint64_t checksum = fnv_offset;
    std::uint64_t total_size = 0u;
    const auto append_byte = [&checksum](const std::uint8_t byte) {
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

/// @brief Exact facts captured for one vkCreateComputePipelines call.  A receipt is
/// accepted only when every Metal pipeline call made under its explicit scope
/// replaced the descriptor archive set, forced fail-on-miss, and first proved
/// that the same descriptor misses against a fresh empty archive.
struct runtime_metal_archive_pipeline_audit {
    std::uint64_t receipt_identity = 0u;
    std::uint64_t scope_identity = 0u;
    std::string cache_digest;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    std::uint64_t seed_receipt_identity = 0u;
    std::uint64_t seed_load_count = 0u;
    std::uint64_t seed_manifest_read_count = 0u;
    std::uint64_t seed_artifact_read_count = 0u;
    std::uint64_t seed_manifest_bytes = 0u;
    std::uint64_t seed_artifact_bytes = 0u;
    std::uint64_t seed_load_ns = 0u;
    std::uint64_t seed_admission_count = 0u;
    std::uint64_t seed_peak_admitted_bytes = 0u;
    std::uint64_t seed_retained_capacity_bytes = 0u;
    bool seed_admission_saturated = true;
    runtime_metal_archive_process_qualification cache_qualification{};
    shader_variant_key shader_key{};
    std::string expected_entry_name;
    std::string observed_entry_name;
    std::uint64_t matched_descriptor_functions = 0u;
    runtime_metal_archive_set_identity expected_archives{};
    runtime_metal_archive_set_identity loaded_archives{};
    std::uint64_t archive_pre_post_identity_checks = 0u;
    std::uint64_t pipeline_pre_identity_checks = 0u;
    std::uint64_t pipeline_post_identity_checks = 0u;
    std::uint64_t unsealed_metal_source_requests = 0u;
    std::uint64_t metal_source_fallback_calls = 0u;
    bool command_interpose_installed = false;
    bool command_interpose_immutable_fail_closed = false;
    std::uint64_t command_loader_metal_registry_id = 0u;
    std::uint64_t command_source_methods_present = 0u;
    std::uint64_t command_source_wrapper_methods = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> configured_vulkan_device_uuid{};
    std::array<std::uint8_t, VK_UUID_SIZE> runtime_vulkan_device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::uint64_t pipeline_create_ns = 0u;
    std::uint64_t expected_metal_registry_id = 0u;
    std::uint64_t loader_metal_registry_id = 0u;
    std::uint64_t intercepted_metal_registry_id = 0u;
    std::uint64_t scoped_interceptions = 0u;
    std::uint64_t descriptor_replacements = 0u;
    std::uint64_t fail_on_miss_enforcements = 0u;
    std::uint64_t same_descriptor_negative_controls = 0u;
    std::uint64_t negative_control_misses = 0u;
    std::string expected_negative_error_domain;
    std::int64_t expected_negative_error_code = 0;
    std::string observed_negative_error_domain;
    std::int64_t observed_negative_error_code = 0;
    std::uint64_t original_sync_negative_calls = 0u;
    std::uint64_t actual_sync_calls = 0u;
    std::uint64_t actual_async_calls = 0u;
    std::uint64_t outside_scope_interceptions = 0u;
    bool nested_scope = false;
    bool sticky_failure = false;
    bool outer_vulkan_pipeline_succeeded = false;

    /// @brief Checks the complete seed, archive, device, interception, and miss-control record.
    /// @return Whether the pipeline call proved immutable archive-only execution.
    [[nodiscard]] bool is_internally_consistent() const noexcept {
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
/// @brief Transactional claim builder for runtime cache metrics.
class runtime_gpu_cache_metrics_claim_plan;
} // namespace runtime_detail

/// @brief Move-only immutable receipt retained by the canonical Vulkan pipeline cache
/// entry. Cache views share that same object. They cannot mint another receipt.
class runtime_metal_archive_pipeline_receipt {
  public:
    /// @brief Creates an empty invalid receipt.
    runtime_metal_archive_pipeline_receipt() = default;

    /// @brief Prevents duplicating one-shot metrics authority by copying.
    runtime_metal_archive_pipeline_receipt(const runtime_metal_archive_pipeline_receipt&) = delete;

    /// @brief Prevents copy assignment of one-shot metrics authority.
    runtime_metal_archive_pipeline_receipt&
    operator=(const runtime_metal_archive_pipeline_receipt&) = delete;

    /// @brief Transfers the audit and one-shot metrics claim state.
    /// @param[in,out] other Receipt whose audit and claim state are consumed.
    runtime_metal_archive_pipeline_receipt(runtime_metal_archive_pipeline_receipt&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          metrics_claimed_(std::exchange(other.metrics_claimed_, false)) {}

    /// @brief Replaces this receipt by transferring another receipt's authority.
    /// @param[in,out] other Receipt whose audit and claim state are consumed.
    /// @return This receipt.
    runtime_metal_archive_pipeline_receipt&
    operator=(runtime_metal_archive_pipeline_receipt&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            metrics_claimed_ = std::exchange(other.metrics_claimed_, false);
        }
        return *this;
    }

    /// @brief Checks the immutable pipeline audit.
    /// @return Whether every pipeline archive proof is internally consistent.
    [[nodiscard]] bool is_valid() const noexcept {
        return audit_.is_internally_consistent();
    }

    /// @brief Returns the immutable pipeline audit.
    /// @return Borrowed audit view valid for this receipt's lifetime.
    [[nodiscard]] const runtime_metal_archive_pipeline_audit& audit() const noexcept {
        return audit_;
    }

    /// @brief Measures storage owned directly by this receipt.
    /// @return Saturating owned-capacity total in bytes.
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

    /// @brief Checks move invalidation and shared retention of one accepted receipt.
    /// @param[in] audit Valid audit used to mint the test receipt.
    /// @return `true` when the move and shared-retention contract holds.
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
    /// @brief Mints one receipt from a completed immutable pipeline audit.
    /// @param[in] audit Pipeline audit transferred into the receipt.
    explicit runtime_metal_archive_pipeline_receipt(
        runtime_metal_archive_pipeline_audit audit) noexcept
        : audit_(std::move(audit)) {}

    runtime_metal_archive_pipeline_audit audit_{};
    mutable bool metrics_claimed_ = false;
    friend class runtime;
    friend class runtime_pipeline_operation_token;
    friend class runtime_detail::runtime_gpu_cache_metrics_claim_plan;
};

static_assert(!std::is_copy_constructible_v<runtime_metal_archive_pipeline_receipt>);
static_assert(std::is_nothrow_move_constructible_v<runtime_metal_archive_pipeline_receipt>);

/// @brief Install-once production seam.  The supervisor calls this from its runtime
/// prepare hook, before the first runtime singleton construction.
/// @param[in] configuration Validated immutable configuration image to install.
/// @return Success or a configuration identity or admission diagnostic.
[[nodiscard]] std::expected<void, std::string> install_runtime_metal_archive_configuration(
    std::shared_ptr<const runtime_metal_archive_configuration_image> configuration);

/// @brief Builds an immutable runtime archive configuration from a sealed seed image.
/// @param[in] seed_image Accepted process-resident shader-cache seed image.
/// @param[in] vulkan_device_uuid Optional additional Vulkan-device identity constraint.
/// @param[in] metal_device_registry_id Optional additional Metal registry constraint.
/// @return Shared validated configuration or its exact validation diagnostic.
[[nodiscard]] std::expected<std::shared_ptr<const runtime_metal_archive_configuration_image>,
                            std::string>
make_runtime_metal_archive_configuration_image(
    std::shared_ptr<const sealed_shader_cache_seed_image> seed_image,
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid = {},
    std::uint64_t metal_device_registry_id = 0u);

/// @brief Reports whether the process-wide archive configuration passed installation.
/// @return Whether an accepted immutable configuration is installed.
[[nodiscard]] bool is_runtime_metal_archive_configuration_installed() noexcept;

} // namespace bioseq::gpu::vulkan

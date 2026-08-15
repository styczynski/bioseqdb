/**
 * @file configuration.cppm
 * @brief Owns install-once Metal archive admission and configuration snapshots.
 */
module;

#include <cstdint>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:metal_archive.configuration;

import :receipts;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

export namespace bioseq::gpu::vulkan {

/// @brief Install-once state and live-gate counters for the admitted archive image.
struct runtime_metal_archive_configuration_slot {
    std::mutex mu;
    std::shared_ptr<const runtime_metal_archive_configuration_image> configuration;
    bool sealed = false;
    std::atomic<bool> installed_accepted{false};
    std::atomic<std::uint64_t> install_validation_count{0u};
    std::atomic<std::uint64_t> live_gate_calls{0u};
    std::atomic<std::uint64_t> live_configuration_scan_count{0u};
};

/// @brief Returns the process-owned archive configuration slot.
runtime_metal_archive_configuration_slot&
runtime_metal_archive_configuration_slot_instance() noexcept {
    static runtime_metal_archive_configuration_slot slot;
    return slot;
}

/// @brief Captures the immutable configuration image under the slot mutex.
[[nodiscard]] std::shared_ptr<const runtime_metal_archive_configuration_image>
runtime_metal_archive_configuration_snapshot() {
    auto& slot = runtime_metal_archive_configuration_slot_instance();
    std::lock_guard<std::mutex> guard(slot.mu);
    return slot.configuration;
}

/// @brief Validates and admits a sealed archive image against exact device expectations.
std::expected<std::shared_ptr<const runtime_metal_archive_configuration_image>, std::string>
make_runtime_metal_archive_configuration_image(
    std::shared_ptr<const sealed_shader_cache_seed_image> seed_image,
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid,
    std::uint64_t metal_device_registry_id) {
    if (!seed_image || !seed_image->is_accepted()) {
        return std::unexpected("runtime Metal archive configuration has no accepted seed image");
    }
    std::uint64_t archive_count = 0u;
    for (const sealed_shader_cache_artifact_image& artifact : seed_image->artifacts()) {
        archive_count +=
            artifact.descriptor.kind == sealed_shader_cache_artifact_kind::metal_binary_archive
                ? 1u
                : 0u;
    }
    if (archive_count == 0u ||
        archive_count > sealed_shader_cache_detail::k_maximum_seed_image_artifacts) {
        return std::unexpected("runtime Metal archive configuration archive count is invalid");
    }
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    std::uint64_t retained = sizeof(runtime_metal_archive_configuration_image);
    retained = saturating_add(
        saturating_add(
            retained,
            saturating_mul(
                archive_count,
                sizeof(runtime_metal_archive_spec) +
                    2u * (sealed_shader_cache_detail::k_maximum_manifest_filename_bytes + 1024u))),
        2u * 1024u);
    const std::uint64_t peak =
        saturating_mul(retained, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
        peak == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected("runtime Metal archive configuration admission saturated");
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
        peak, &admission_failure);
    if (!permit) {
        return std::unexpected(shader_cache_message(
            "runtime Metal archive configuration admission denied before allocation: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(admission_failure.reason),
            " requested=",
            admission_failure.requested_bytes));
    }
    try {
        runtime_metal_archive_configuration configuration{};
        configuration.seed_image = std::move(seed_image);
        configuration.vulkan_device_uuid = vulkan_device_uuid;
        configuration.metal_device_registry_id = metal_device_registry_id;
        configuration.archives.reserve(static_cast<std::size_t>(archive_count));
        std::optional<metal_archive_negative_error_fingerprint> common_error;
        for (std::size_t ordinal = 0u; ordinal < configuration.seed_image->artifacts().size();
             ++ordinal) {
            const sealed_shader_cache_artifact_image& artifact =
                configuration.seed_image->artifacts()[ordinal];
            if (artifact.descriptor.kind !=
                sealed_shader_cache_artifact_kind::metal_binary_archive) {
                continue;
            }
            const std::string qualification_filename =
                artifact.descriptor.filename + ".qualification";
            const auto* qualification_image = sealed_shader_cache_detail::find_artifact_image(
                configuration.seed_image->artifacts(),
                sealed_shader_cache_artifact_kind::metal_binary_archive_qualification,
                artifact.descriptor.key_index,
                qualification_filename);
            if (qualification_image == nullptr) {
                return std::unexpected(
                    shader_cache_message("runtime Metal archive qualification missing for ",
                                         artifact.descriptor.filename));
            }
            auto qualification =
                sealed_shader_cache_detail::parse_metal_archive_qualification_receipt(
                    qualification_image->bytes);
            if (!qualification || !qualification->is_production_accepted() ||
                qualification->archive_size != artifact.descriptor.size ||
                qualification->archive_sha256 != artifact.descriptor.sha256) {
                return std::unexpected(
                    shader_cache_message("runtime Metal archive qualification mismatch for ",
                                         artifact.descriptor.filename));
            }
            if (common_error && *common_error != qualification->negative_error) {
                return std::unexpected(
                    "runtime Metal archive qualifications disagree on NSError fingerprint");
            }
            common_error = qualification->negative_error;
            if (configuration.qualification_receipt_count == 0u) {
                configuration.qualification_wire_revision = qualification->wire_revision;
                configuration.helper_wire_revision = qualification->helper_wire_revision;
                configuration.helper_build_identity = qualification->helper_build_identity;
                configuration.helper_source = qualification->helper_source;
                configuration.helper_executable_size = qualification->helper_executable_size;
                configuration.helper_executable_sha256 = qualification->helper_executable_sha256;
                configuration.helper_metal_registry_id = qualification->helper_metal_registry_id;
            } else if (configuration.qualification_wire_revision != qualification->wire_revision ||
                       configuration.helper_wire_revision != qualification->helper_wire_revision ||
                       configuration.helper_build_identity !=
                           qualification->helper_build_identity ||
                       configuration.helper_source != qualification->helper_source ||
                       configuration.helper_executable_size !=
                           qualification->helper_executable_size ||
                       configuration.helper_executable_sha256 !=
                           qualification->helper_executable_sha256 ||
                       configuration.helper_metal_registry_id !=
                           qualification->helper_metal_registry_id) {
                return std::unexpected(
                    "runtime Metal archive qualifications disagree on helper provenance");
            }
            ++configuration.qualification_receipt_count;
            configuration.archives.push_back(runtime_metal_archive_spec{
                .artifact_ordinal = static_cast<std::uint32_t>(ordinal),
                .artifact_filename = artifact.descriptor.filename,
                .size = artifact.descriptor.size,
                .sha256 = artifact.descriptor.sha256,
                .negative_error_domain = std::move(qualification->negative_error.domain),
                .negative_error_code = qualification->negative_error.code,
            });
        }
        if (!configuration.is_valid()) {
            return std::unexpected(
                "runtime Metal archive configuration factory produced invalid evidence");
        }
        auto installed = std::shared_ptr<runtime_metal_archive_configuration_image>(
            new runtime_metal_archive_configuration_image(
                std::move(configuration), peak, std::move(*permit)));
        if (!installed->reconcile() || !installed->is_valid()) {
            return std::unexpected(
                "runtime Metal archive configuration admission reconciliation failed");
        }
        return installed;
    } catch (const std::bad_alloc&) {
        return std::unexpected("runtime Metal archive configuration allocation failed");
    }
}

/// @brief Installs one admitted configuration before runtime singleton construction.
std::expected<void, std::string> install_runtime_metal_archive_configuration(
    std::shared_ptr<const runtime_metal_archive_configuration_image> configuration) {
    if (!configuration || !configuration->is_valid()) {
        return std::unexpected(
            "runtime Metal archive configuration is incomplete or non-canonical");
    }
    auto& slot = runtime_metal_archive_configuration_slot_instance();
    slot.install_validation_count.fetch_add(1u, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(slot.mu);
    if (slot.sealed) {
        if (slot.configuration &&
            slot.configuration->configuration() == configuration->configuration()) {
            return {};
        }
        return std::unexpected(
            "runtime Metal archive configuration is already sealed to different evidence");
    }
    slot.configuration = std::move(configuration);
    slot.sealed = true;
    slot.installed_accepted.store(true, std::memory_order_release);
    return {};
}

/// @brief Reports whether immutable archive configuration passed install validation.
bool is_runtime_metal_archive_configuration_installed() noexcept {
    auto& slot = runtime_metal_archive_configuration_slot_instance();
    return slot.installed_accepted.load(std::memory_order_acquire);
}

/// @brief Captures install and live-gate counters without rescanning archive bytes.
[[nodiscard]] runtime_metal_archive_live_gate_result
capture_runtime_metal_archive_live_gate_result() noexcept {
    auto& slot = runtime_metal_archive_configuration_slot_instance();
    return runtime_metal_archive_live_gate_result{
        .install_validation_count = slot.install_validation_count.load(std::memory_order_relaxed),
        .live_gate_calls = slot.live_gate_calls.load(std::memory_order_relaxed),
        .live_configuration_scan_count =
            slot.live_configuration_scan_count.load(std::memory_order_relaxed),
        .installed_accepted = slot.installed_accepted.load(std::memory_order_acquire),
    };
}

} // namespace bioseq::gpu::vulkan

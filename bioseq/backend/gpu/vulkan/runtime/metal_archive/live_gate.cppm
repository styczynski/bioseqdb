/**
 * @file live_gate.cppm
 * @brief Validates live immutable Metal policy and private archive snapshots.
 */
module;

#include <fcntl.h>
#include <unistd.h>

export module bioseq.gpu.vulkan.runtime:metal_archive.live_gate;

import :receipts;
import :metal_archive.configuration;
import :metal_archive.interpose;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

export namespace bioseq::gpu::vulkan {

/**
 * @brief Fails when live interposition state diverges from immutable qualification.
 *
 * On Apple platforms, compares archive and command interposition state against
 * the installed configuration and process qualification. Non-Apple builds are a
 * no-op. Increments the slot live-gate counter on every call.
 *
 * @param[in] operation_label Label included in the thrown diagnostic on failure.
 * @throws vulkan_runtime_error On Apple platforms when interposition state is stale or poisoned.
 */
void require_immutable_metal_policy_live(std::string_view operation_label) {
    runtime_metal_archive_configuration_slot_instance().live_gate_calls.fetch_add(
        1u, std::memory_order_relaxed);
#if defined(__APPLE__)
    if (!is_runtime_metal_archive_configuration_installed())
        return;
    auto& archive_state = metal_pipeline_archive_interpose_state();
    auto& command_state = moltenvk_command_interpose_state();
    const bool clean =
        archive_state.configuration_validated && archive_state.configured_identity.is_valid() &&
        archive_state.configured_identity == archive_state.loaded_identity &&
        archive_state.installed && archive_state.archive_array != nullptr &&
        archive_state.process_qualification.is_internally_consistent() &&
        archive_state.process_qualification.archive_set == archive_state.loaded_identity &&
        !archive_state.sticky_failure.load(std::memory_order_acquire) &&
        archive_state.outside_scope_interceptions.load(std::memory_order_relaxed) == 0u &&
        command_state.installed && command_state.immutable_fail_closed &&
        command_state.cached_binding_validated &&
        !command_state.sticky_failure.load(std::memory_order_acquire) &&
        command_state.recognized_source_requests.load(std::memory_order_relaxed) == 0u &&
        command_state.source_fallback_calls.load(std::memory_order_relaxed) == 0u &&
        archive_state.loader_registry_id == archive_state.runtime_metal_registry_id &&
        archive_state.loader_registry_id ==
            archive_state.process_qualification.helper_metal_registry_id &&
        archive_state.runtime_vulkan_device_identity.device_uuid ==
            archive_state.process_qualification.vulkan_device_uuid &&
        archive_state.runtime_vulkan_device_identity.api_version ==
            archive_state.process_qualification.api_version &&
        archive_state.runtime_vulkan_device_identity.driver_version ==
            archive_state.process_qualification.driver_version &&
        archive_state.runtime_vulkan_device_identity.vendor_id ==
            archive_state.process_qualification.vendor_id &&
        archive_state.runtime_vulkan_device_identity.device_id ==
            archive_state.process_qualification.device_id &&
        archive_state.runtime_vulkan_device_identity.pipeline_cache_uuid ==
            archive_state.process_qualification.pipeline_cache_uuid &&
        command_state.loader_registry_id == archive_state.loader_registry_id &&
        command_state.cached_methods_present == 2u &&
        command_state.cached_wrapper_methods == command_state.cached_methods_present;
    if (!clean) {
        throw vulkan_runtime_error(fmt::format(
            "{} observed stale or poisoned immutable Metal policy state", operation_label));
    }
#else
    (void)operation_label;
#endif
}

/**
 * @brief Executes the production live gate through the test-only public seam.
 *
 * Invokes @ref require_immutable_metal_policy_live with a fixed unit-test label.
 */
void probe_immutable_metal_policy_live_for_test() {
    require_immutable_metal_policy_live("unit-test live gate");
}

/**
 * @brief Creates, verifies, tampers with, and rejects one private archive snapshot.
 *
 * Writes a private snapshot, captures it against expected bytes, mutates the
 * on-disk file, and confirms the second capture fails. Apple-only.
 *
 * @param[in] artifact_filename Stable archive artifact filename for the spec.
 * @param[in] bytes Immutable expected archive bytes used for digest comparison.
 * @param[in] temp_parent Parent directory for the private validation snapshot.
 * @return Capture metrics and tamper-rejection flag, or a diagnostic on failure.
 */
[[nodiscard]] std::expected<runtime_private_archive_snapshot_test_result, std::string>
verify_runtime_private_archive_snapshot_for_test(std::string_view artifact_filename,
                                                 std::span<const std::byte> bytes,
                                                 const std::filesystem::path& temp_parent) {
#if defined(__APPLE__)
    if (artifact_filename.empty() || bytes.empty()) {
        return std::unexpected("private archive snapshot test input is empty");
    }
    runtime_metal_archive_spec expected{
        .artifact_ordinal = 0u,
        .artifact_filename = std::string(artifact_filename),
        .size = static_cast<std::uint64_t>(bytes.size()),
        .sha256 = sealed_shader_cache_detail::sha256(bytes),
        .negative_error_domain = "MTLLibraryErrorDomain",
        .negative_error_code = 6,
    };
    auto private_image =
        write_private_metal_archive_snapshot(artifact_filename, bytes, temp_parent);
    if (!private_image)
        return std::unexpected(private_image.error());
    auto snapshot = capture_metal_archive_file_snapshot(private_image->path(), expected, bytes);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    const int fd = ::open(private_image->path().c_str(), O_WRONLY);
    if (fd < 0) {
        return std::unexpected("cannot open private snapshot for tamper test");
    }
    const std::byte changed = bytes.front() ^ std::byte{0x01};
    const bool wrote = ::pwrite(fd, &changed, sizeof(changed), 0) == 1 && ::fsync(fd) == 0;
    (void)::close(fd);
    if (!wrote) {
        return std::unexpected("cannot mutate private snapshot for tamper test");
    }
    const auto rejected =
        capture_metal_archive_file_snapshot(private_image->path(), expected, bytes);
    return runtime_private_archive_snapshot_test_result{
        .snapshot_path = private_image->path(),
        .streamed_bytes = snapshot->streamed_bytes,
        .scratch_capacity_bytes = snapshot->scratch_capacity_bytes,
        .exact_seed_compared = snapshot->exact_seed_compared,
        .tamper_rejected = !rejected,
    };
#else
    (void)artifact_filename;
    (void)bytes;
    (void)temp_parent;
    return std::unexpected("private Metal archive snapshots are Apple-only");
#endif
}

} // namespace bioseq::gpu::vulkan

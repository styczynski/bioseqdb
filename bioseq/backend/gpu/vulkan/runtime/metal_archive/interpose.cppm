/**
 * @file interpose.cppm
 * @brief Tracks Metal archive checks while a Vulkan pipeline is created.
 */
module;

#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <libassert/assert.hpp>
#include <new>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:metal_archive.interpose;

export import :metal_archive.command_interpose;
export import :metal_archive.archive_storage;
export import :metal_archive.pipeline_interpose;
import :receipts;
import :metal_archive.configuration;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

export namespace bioseq::gpu::vulkan {

#if defined(__APPLE__)
/// @brief Collects the Metal checks belonging to one Vulkan pipeline creation.
class metal_archive_pipeline_create_scope {
  public:
    /// @brief Starts tracking one pipeline creation against the installed Metal archives.
    /// @param[in] required Whether creation must produce an accepted archive record.
    /// @param[in] runtime_identity Selected Vulkan device and driver details.
    /// @param[in] shader_key Shader  variant being used.
    /// @param[in] expected_entry_name Metal function the pipeline must use.
    metal_archive_pipeline_create_scope(bool required,
                                        const runtime_vulkan_device_identity& runtime_identity,
                                        shader_variant_key shader_key,
                                        std::string expected_entry_name)
        : required_(required) {
        auto& state = metal_pipeline_archive_interpose_state();
        if (!is_runtime_metal_archive_configuration_installed() || !state.configuration_image) {
            if (required_) {
                failure_ =
                    "provider-backed Vulkan pipeline has no sealed Metal archive configuration";
            }
            return;
        }
        const runtime_metal_archive_configuration& configuration =
            state.configuration_image->configuration();
        const sealed_shader_cache_manifest& manifest = configuration.seed_image->manifest();
        const sealed_shader_cache_seed_receipt& seed_receipt = configuration.seed_image->receipt();
        audit_.cache_digest = manifest.cache_digest;
        audit_.seed_sha256 = manifest.seed_sha256;
        audit_.seed_size = manifest.seed_size;
        audit_.manifest_checksum = manifest.manifest_checksum;
        audit_.seed_receipt_identity = seed_receipt.identity();
        audit_.seed_load_count = seed_receipt.load_count();
        audit_.seed_manifest_read_count = seed_receipt.manifest_read_count();
        audit_.seed_artifact_read_count = seed_receipt.artifact_read_count();
        audit_.seed_manifest_bytes = seed_receipt.manifest_bytes();
        audit_.seed_artifact_bytes = seed_receipt.artifact_bytes();
        audit_.seed_load_ns = seed_receipt.load_ns();
        audit_.seed_admission_count = seed_receipt.admission_count();
        audit_.seed_peak_admitted_bytes = seed_receipt.peak_admitted_bytes();
        audit_.seed_retained_capacity_bytes = seed_receipt.retained_capacity_bytes();
        audit_.seed_admission_saturated = seed_receipt.admission_saturated();
        audit_.cache_qualification = state.process_qualification;
        audit_.shader_key = shader_key;
        audit_.expected_entry_name = std::move(expected_entry_name);
        audit_.expected_archives = make_runtime_metal_archive_set_identity(configuration.archives);
        audit_.loaded_archives = state.loaded_identity;
        audit_.expected_negative_error_domain =
            configuration.archives.front().negative_error_domain;
        audit_.expected_negative_error_code = configuration.archives.front().negative_error_code;
        audit_.archive_pre_post_identity_checks = state.loaded_pre_post_identity_checks;
        auto& command_state = moltenvk_command_interpose_state();
        audit_.command_interpose_installed = command_state.installed;
        audit_.command_interpose_immutable_fail_closed = command_state.immutable_fail_closed;
        audit_.command_loader_metal_registry_id = command_state.loader_registry_id;
        audit_.unsealed_metal_source_requests =
            command_state.recognized_source_requests.load(std::memory_order_relaxed);
        audit_.metal_source_fallback_calls =
            command_state.source_fallback_calls.load(std::memory_order_relaxed);
        audit_.configured_vulkan_device_uuid = state.runtime_vulkan_device_uuid;
        audit_.runtime_vulkan_device_uuid = runtime_identity.device_uuid;
        audit_.api_version = runtime_identity.api_version;
        audit_.driver_version = runtime_identity.driver_version;
        audit_.vendor_id = runtime_identity.vendor_id;
        audit_.device_id = runtime_identity.device_id;
        audit_.pipeline_cache_uuid = runtime_identity.pipeline_cache_uuid;
        audit_.expected_metal_registry_id = state.runtime_metal_registry_id;
        audit_.loader_metal_registry_id = state.loader_registry_id;
        audit_.scope_identity = next_nonzero_identity(state.next_scope_identity);
        if (audit_.scope_identity == 0u) {
            failure_ = "Metal archive pipeline scope identity space exhausted";
            return;
        }
        if (!state.installed || state.archive_array == nullptr ||
            !audit_.cache_qualification.is_internally_consistent()) {
            failure_ = "sealed Metal archive interpose is not installed";
            return;
        }
        if (state.sticky_failure.load(std::memory_order_acquire)) {
            audit_.sticky_failure = true;
            failure_ = metal_archive_failure_reason(state);
            return;
        }
        if (!audit_.command_interpose_installed ||
            !audit_.command_interpose_immutable_fail_closed) {
            failure_ = "immutable MoltenVK command-source interpose is not installed fail-closed";
            mark_metal_archive_sticky_failure(state, failure_);
            return;
        }
        if (command_state.sticky_failure.load(std::memory_order_acquire) ||
            audit_.unsealed_metal_source_requests != 0u ||
            audit_.metal_source_fallback_calls != 0u) {
            failure_ = "immutable GPU runtime observed an unsealed Metal source-library request";
            mark_metal_archive_sticky_failure(state, failure_);
            return;
        }
        if (audit_.configured_vulkan_device_uuid != audit_.runtime_vulkan_device_uuid) {
            failure_ = "runtime Vulkan UUID differs from sealed archive UUID";
            mark_metal_archive_sticky_failure(state, failure_);
            return;
        }
        active_ = std::make_shared<active_metal_archive_pipeline_scope>();
        active_->audit = audit_;
        active_->required = required_;
        active_->active = true;
        {
            std::lock_guard<std::mutex> guard(state.active_scope_mu);
            if (state.active_scope != nullptr) {
                audit_.nested_scope = true;
                failure_ = "nested Metal archive pipeline-create scope rejected";
                mark_metal_archive_sticky_failure(state, failure_);
                active_.reset();
                return;
            }
            active_->outside_scope_interceptions_at_begin =
                state.outside_scope_interceptions.load(std::memory_order_relaxed);
            state.active_scope = active_;
        }
        active_scope_ = true;
    }

    /// @brief Prevents copying an active pipeline-creation scope.
    metal_archive_pipeline_create_scope(const metal_archive_pipeline_create_scope&) = delete;

    /// @brief Prevents copying an active scope by assignment.
    metal_archive_pipeline_create_scope&
    operator=(const metal_archive_pipeline_create_scope&) = delete;

    /// @brief Records a process-wide failure if an active scope ends before finish().
    ~metal_archive_pipeline_create_scope() noexcept {
        if (!active_scope_ || active_ == nullptr)
            return;
        auto& state = metal_pipeline_archive_interpose_state();
        try {
            std::unique_lock<std::mutex> active_guard(state.active_scope_mu);
            try {
                std::unique_lock<std::mutex> scope_guard(active_->mu);
                if (state.active_scope == active_)
                    state.active_scope.reset();
                active_->active = false;
                active_->finished = true;
                if (!finished_) {
                    mark_metal_archive_sticky_failure_literal(
                        state, "Metal archive pipeline-create scope abandoned before verdict");
                }
            } catch (...) {
                if (state.active_scope == active_)
                    state.active_scope.reset();
                mark_metal_archive_sticky_failure_literal(
                    state, "Metal archive pipeline-create scope cleanup failed");
            }
        } catch (...) {
            mark_metal_archive_sticky_failure_literal(
                state, "Metal archive pipeline-create scope cleanup failed");
        }
    }

    /// @brief Returns whether setup succeeded and pipeline creaion may proceed
    [[nodiscard]] bool is_ready_for_pipeline_call() const noexcept {
        return failure_.empty() && (active_scope_ || !required_);
    }

    /// @brief Ends tracking and checks that this pipeline used the accepted archives.
    /// @param[in] outer_result Result of vkCreateComputePipelines.
    /// @param[in] pipeline_create_ns Time spent creating the pipeline, in nanoseconds.
    /// @return Accepted creation details, an empty result if no archive scope was needed,
    /// or an  error describing the failed check.
    [[nodiscard]] std::expected<std::optional<runtime_metal_archive_pipeline_audit>, std::string>
    finish(VkResult outer_result, std::uint64_t pipeline_create_ns) {
        if (finished_) {
            return std::unexpected("Metal archive pipeline-create scope verdict consumed twice");
        }
        finished_ = true;
        if (!failure_.empty())
            return std::unexpected(failure_);
        if (!active_scope_) {
            if (required_) {
                return std::unexpected(
                    "provider-backed Vulkan pipeline did not enter immutable Metal archive scope");
            }
            return std::optional<runtime_metal_archive_pipeline_audit>{};
        }
        auto& state = metal_pipeline_archive_interpose_state();
        {
            std::unique_lock<std::mutex> active_guard(state.active_scope_mu);
            std::unique_lock<std::mutex> scope_guard(active_->mu);
            if (state.active_scope != active_) {
                return std::unexpected(
                    "Metal archive pipeline-create scope ownership changed before verdict");
            }
            const std::uint64_t outside_end =
                state.outside_scope_interceptions.load(std::memory_order_relaxed);
            if (outside_end < active_->outside_scope_interceptions_at_begin) {
                active_->audit.outside_scope_interceptions =
                    (std::numeric_limits<std::uint64_t>::max)();
            } else {
                active_->audit.outside_scope_interceptions =
                    outside_end - active_->outside_scope_interceptions_at_begin;
            }
            active_->active = false;
            active_->finished = true;
            state.active_scope.reset();
            audit_ = std::move(active_->audit);
            if (!active_->failure.empty())
                failure_ = active_->failure;
        }
        if (!failure_.empty())
            return std::unexpected(failure_);
        if (!state.process_qualification.is_internally_consistent() ||
            state.process_qualification != audit_.cache_qualification) {
            failure_ = "process Metal archive qualification changed after pipeline";
            mark_metal_archive_sticky_failure(state, failure_);
            return std::unexpected(failure_);
        }
        audit_.pipeline_post_identity_checks = 0u;
        auto& command_state = moltenvk_command_interpose_state();
        audit_.unsealed_metal_source_requests =
            command_state.recognized_source_requests.load(std::memory_order_relaxed);
        audit_.metal_source_fallback_calls =
            command_state.source_fallback_calls.load(std::memory_order_relaxed);
        audit_.command_interpose_installed = command_state.installed;
        audit_.command_interpose_immutable_fail_closed = command_state.immutable_fail_closed;
        audit_.sticky_failure =
            audit_.sticky_failure || state.sticky_failure.load(std::memory_order_acquire);
        audit_.sticky_failure =
            audit_.sticky_failure || command_state.sticky_failure.load(std::memory_order_acquire);
        audit_.outer_vulkan_pipeline_succeeded = outer_result == VK_SUCCESS;
        audit_.pipeline_create_ns = pipeline_create_ns;
        audit_.receipt_identity = next_nonzero_identity(state.next_receipt_identity);
        if (!audit_.is_internally_consistent()) {
            return std::unexpected(
                fmt::format("immutable Metal archive receipt rejected: vk={} calls={} replace={} "
                            "enforce={} negative={}/{} sticky={}",
                            static_cast<int>(outer_result),
                            audit_.scoped_interceptions,
                            audit_.descriptor_replacements,
                            audit_.fail_on_miss_enforcements,
                            audit_.negative_control_misses,
                            audit_.same_descriptor_negative_controls,
                            audit_.sticky_failure));
        }
        return std::optional<runtime_metal_archive_pipeline_audit>{std::move(audit_)};
    }

  private:
    bool required_ = false;
    bool active_scope_ = false;
    bool finished_ = false;
    std::string failure_;
    runtime_metal_archive_pipeline_audit audit_{};
    std::shared_ptr<active_metal_archive_pipeline_scope> active_{};
};
#else
/// @brief Does nothing on platforms that do not use MoltenVK command libraries.
void install_moltenvk_command_library_interpose() noexcept {}
/// @brief Does nothing on platforms that do not use Metal archives.
void install_metal_pipeline_archive_interpose(runtime_vulkan_device_identity) noexcept {}

/// @brief Rejects required Metal archive checks on platforms without Metal.
class metal_archive_pipeline_create_scope {
  public:
    /// @brief Records whether this non-Apple call would require Metal archive checks.
    /// @param[in] required Whether the caller requires an archive record.
    /// Installed Metal settings also make the checks required. Other constructor
    /// arguments are unused on this platform.
    metal_archive_pipeline_create_scope(bool required,
                                        const runtime_vulkan_device_identity&,
                                        shader_variant_key,
                                        std::string) noexcept
        : required_(required || is_runtime_metal_archive_configuration_installed()) {}

    /// @brief Returns whether creation, can proceed without Metal archive checks.
    [[nodiscard]] bool is_ready_for_pipeline_call() const noexcept {
        return !required_;
    }

    /// @brief Returns an empty record if Metal checks were optional, or a  platform error.
    [[nodiscard]] std::expected<std::optional<runtime_metal_archive_pipeline_audit>, std::string>
    finish(VkResult, std::uint64_t) const {
        if (required_) {
            return std::unexpected(
                "immutable Metal archive receipt requested on a non-Apple runtime");
        }
        return std::optional<runtime_metal_archive_pipeline_audit>{};
    }

  private:
    bool required_ = false;
};
#endif

} // namespace bioseq::gpu::vulkan

/**
 * @file receipts.cppm
 * @brief Defines Vulkan ownership, archive, cache, and operation receipts.
 */
module;

#include <cstddef>
#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:receipts;

export import bioseq.gpu.vulkan.shader_cache;
export import bioseq.gpu.vulkan.shader_layout;
export import bioseq.gpu.vulkan.shader_variants;
export import bioseq.gpu.vulkan.abi;
export import bioseq.prefilter.types;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.instrumentation;
import std;

export namespace bioseq::gpu::vulkan {

/// @brief Process-wide owner of the Vulkan device, queues, pipelines, and buffers.
class runtime;

#ifdef BIOSEQ_TEST_INDEXES
/** @brief Test-build-only latch around the real GPU frontend upload/submit/fence path.
 * Normal builds do not contain this API or its branches. */
enum class vulkan_submission_test_fault : std::uint8_t {
    none,
    cancellation_after_submit,
    upload_failure_after_submit,
};

/// @brief Observable state of the test-only submission fault seam.
struct vulkan_submission_test_snapshot {
    vulkan_submission_test_fault fault{vulkan_submission_test_fault::none};
    std::uint64_t upload_latches = 0u;
    std::uint64_t submissions = 0u;
    std::uint64_t fence_drains = 0u;
    std::uint64_t delivered_faults = 0u;
    bool waiting_after_submit = false;
    bool waiting_after_drain = false;
    bool cancellation_requested = false;
    bool upload_failure_latched = false;
};

/// @brief Arms one test-only fault for the next Vulkan submission.
void arm_vulkan_submission_test_seam(vulkan_submission_test_fault fault) noexcept;
/// @brief Releases a test submission waiting at its fence seam.
void release_vulkan_submission_test_fence() noexcept;
/// @brief Releases delivery of a fault latched by the test seam.
void release_vulkan_submission_test_delivery() noexcept;
/// @brief Restores the test submission seam to its idle state.
void reset_vulkan_submission_test_seam() noexcept;
/// @brief Captures a consistent snapshot of the test submission seam.
/// @return Current fault, gate, and submission counters.
[[nodiscard]] vulkan_submission_test_snapshot capture_vulkan_submission_test_snapshot() noexcept;
#endif

/// @brief Host-owned results from one batched alignment and CIGAR dispatch.
struct alignment_cigar_batch_result {
    std::vector<gpu_alignment_cigar_result_wire> results{};
    std::vector<std::uint32_t> packed_cigar{};
};

/// @brief Capabilities probed once at init. `available == false` means there is no
/// usable Vulkan device and selected GPU algorithms must fail.
struct vulkan_feature_set {
    bool available = false;
    bool shader_int64 = false;
    bool scalar_block_layout = false;
    bool timeline_semaphore = false;
    bool software_device = false;
    std::uint32_t device_type = 0;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t max_workgroup_invocations = 0;
    std::uint32_t max_workgroup_size_x = 0;
    std::uint32_t max_workgroup_size_y = 0;
    std::uint32_t max_workgroup_size_z = 0;
    std::uint32_t max_workgroup_count_x = 0;
    std::uint32_t max_shared_memory_bytes = 0;
    std::uint64_t max_storage_buffer_range = 0;
    std::uint64_t min_storage_buffer_offset_alignment = 1;
    std::uint64_t device_local_bytes = 0;
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::array<char, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE> device_name{};
};

inline constexpr std::size_t k_rank_prefilter_replay_route_count = 7u;

/// @brief Resolve the fixed recorded-command slot for one rank-prefilter shader and
/// push mode. Keeping this mapping pure makes the seven non-aliasing replay
/// identities independently testable without a Vulkan device.
/// @param[in] kind Rank-prefilter shader identity.
/// @param[in] push_mode Shader push mode encoded in the recorded command.
/// @return The matching replay slot, or no value for an unsupported route.
[[nodiscard]] constexpr std::optional<std::size_t>
rank_prefilter_replay_index(shader_kind kind, std::uint32_t push_mode) noexcept {
    if (kind == shader_kind::sneaky_snake_rank_batch) {
        if (push_mode == 1u)
            return 0u;
        if (push_mode == 8u)
            return 1u;
        return std::nullopt;
    }
    if (push_mode != 0u)
        return std::nullopt;
    switch (kind) {
    case shader_kind::gatekeeper_rank_batch:
        return 2u;
    case shader_kind::gatekeeper_rank_batch_l32_e2:
        return 3u;
    case shader_kind::gatekeeper_rank_batch_l32_e4:
        return 4u;
    case shader_kind::gatekeeper_rank_batch_l64_e2:
        return 5u;
    case shader_kind::gatekeeper_rank_batch_l64_e4:
        return 6u;
    default:
        return std::nullopt;
    }
}

/// @brief Shader identity and replay slot selected for one GateKeeper batch.
struct gatekeeper_rank_batch_route {
    shader_kind kind = shader_kind::gatekeeper_rank_batch;
    std::size_t replay_index = 2u;

    constexpr bool operator==(const gatekeeper_rank_batch_route&) const noexcept = default;
};

/// @brief Select a specialized GateKeeper shader only for a homogeneous, word-aligned
/// production batch. Every other shape deliberately retains the generic key.
/// @param[in] jobs Borrowed prefilter jobs whose geometry selects the route.
/// @return The specialized or generic GateKeeper route.
[[nodiscard]] inline gatekeeper_rank_batch_route resolve_gatekeeper_rank_batch_route(
    std::span<const bioseq::prefilter::prefilter_job> jobs) noexcept {
    if (jobs.empty())
        return {};

    const std::uint32_t length = jobs.front().query_length;
    const std::uint32_t max_edits = jobs.front().max_edits;
    bool homogeneous_aligned_production = jobs.front().target_length == length &&
                                          (length == 32u || length == 64u) &&
                                          (max_edits == 2u || max_edits == 4u);
    for (const auto& job : jobs) {
        homogeneous_aligned_production =
            homogeneous_aligned_production && job.query_length == length &&
            job.target_length == length && job.max_edits == max_edits &&
            (job.query_offset & 3u) == 0u && (job.target_offset & 3u) == 0u;
    }
    if (!homogeneous_aligned_production)
        return {};

    if (length == 32u && max_edits == 2u) {
        return {
            .kind = shader_kind::gatekeeper_rank_batch_l32_e2,
            .replay_index = 3u,
        };
    }
    if (length == 32u) {
        return {
            .kind = shader_kind::gatekeeper_rank_batch_l32_e4,
            .replay_index = 4u,
        };
    }
    if (max_edits == 2u) {
        return {
            .kind = shader_kind::gatekeeper_rank_batch_l64_e2,
            .replay_index = 5u,
        };
    }
    return {
        .kind = shader_kind::gatekeeper_rank_batch_l64_e4,
        .replay_index = 6u,
    };
}

/// @brief Canonical allocation-free Vulkan driver identity used by SQL/report receipt.
/// Numeric fields avoid unstable marketing strings and preserve the exact
/// pipeline-cache compatibility UUID byte order reported by Vulkan.
/// @param[in] features Immutable Vulkan device feature snapshot.
/// @return A fixed-width canonical driver identity string.
[[nodiscard]] inline std::array<char, 128>
canonical_vulkan_driver_identity(const vulkan_feature_set& features) noexcept {
    std::array<char, 128> result{};
    constexpr char hex[] = "0123456789abcdef";
    std::size_t cursor = 0u;
    const auto append_literal = [&result, &cursor](std::string_view value) {
        if (cursor > result.size() - value.size() - 1u)
            return false;
        for (const char ch : value)
            result[cursor++] = ch;
        return true;
    };
    const auto append_u32 = [&result, &cursor](std::uint32_t value) {
        constexpr char digits[] = "0123456789abcdef";
        if (cursor > result.size() - 9u)
            return false;
        for (std::uint32_t shift = 28u;; shift -= 4u) {
            result[cursor++] = digits[(value >> shift) & 0x0fu];
            if (shift == 0u)
                break;
        }
        return true;
    };
    if (!append_literal("api=") || !append_u32(features.api_version) ||
        !append_literal(";driver=") || !append_u32(features.driver_version) ||
        !append_literal(";vendor=") || !append_u32(features.vendor_id) ||
        !append_literal(";device=") || !append_u32(features.device_id) ||
        !append_literal(";pcuuid=")) {
        return {};
    }
    for (const std::uint8_t byte : features.pipeline_cache_uuid) {
        if (cursor > result.size() - 3u)
            return {};
        result[cursor++] = hex[byte >> 4u];
        result[cursor++] = hex[byte & 0x0fu];
    }
    result[cursor] = '\0';
    return result;
}

/// @brief RAII owner for one (VkBuffer, VkDeviceMemory) pair. Move-only. Destruction
/// routes through `runtime::destroy_buffer` (the single cleanup path).
struct buffer {
    buffer() = default;
    buffer(runtime* owner,
           VkBuffer buffer_handle,
           VkDeviceMemory memory,
           std::uint64_t bytes,
           std::uint64_t allocation_bytes,
           VkMemoryPropertyFlags memory_properties,
           bool process_footprint_reflected,
           void* mapped) noexcept
        : owner(owner), buffer_handle(buffer_handle), memory(memory), bytes(bytes),
          allocation_bytes(allocation_bytes), memory_properties(memory_properties),
          process_footprint_reflected(process_footprint_reflected), mapped(mapped) {}
    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;
    buffer(buffer&& other) noexcept {
        move_from(other);
    }
    buffer& operator=(buffer&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }
    ~buffer() {
        reset();
    }

    void reset() noexcept;
    [[nodiscard]] bool is_valid() const noexcept {
        return buffer_handle != VK_NULL_HANDLE;
    }
    [[nodiscard]] bool is_host_visible_coherent() const noexcept {
        constexpr VkMemoryPropertyFlags required =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        return (memory_properties & required) == required;
    }

    runtime* owner = nullptr;
    VkBuffer buffer_handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint64_t bytes = 0;
    std::uint64_t allocation_bytes = 0;
    VkMemoryPropertyFlags memory_properties = 0;
    bool process_footprint_reflected = false;
    void* mapped = nullptr;

  private:
    void move_from(buffer& other) noexcept {
        owner = other.owner;
        buffer_handle = other.buffer_handle;
        memory = other.memory;
        bytes = other.bytes;
        allocation_bytes = other.allocation_bytes;
        memory_properties = other.memory_properties;
        process_footprint_reflected = other.process_footprint_reflected;
        mapped = other.mapped;
        other.owner = nullptr;
        other.buffer_handle = VK_NULL_HANDLE;
        other.memory = VK_NULL_HANDLE;
        other.bytes = 0;
        other.allocation_bytes = 0;
        other.memory_properties = 0;
        other.process_footprint_reflected = false;
        other.mapped = nullptr;
    }
};

/// @brief Converts a Vulkan dispatchable or non-dispatchable handle to stable bits.
/// @param[in] handle Vulkan handle to encode without dereferencing it.
/// @return Stable unsigned handle bits.
template <class p_handle_type>
[[nodiscard]] inline std::uint64_t vulkan_handle_identity(const p_handle_type handle) noexcept {
    static_assert(sizeof(p_handle_type) <= sizeof(std::uint64_t));
    std::uint64_t identity = 0u;
    std::memcpy(&identity, &handle, sizeof(handle));
    return identity;
}

/// @brief Immutable snapshot of one runtime-owned allocation.  The object address is
/// insufficient because buffer is move-assignable. Operation receipt therefore
/// binds its owner, Vulkan allocation handles, and physical/logical sizes too.
struct runtime_buffer_identity {
    const buffer* object = nullptr;
    const runtime* owner = nullptr;
    std::uint64_t buffer_handle = 0u;
    std::uint64_t memory_handle = 0u;
    std::uint64_t bytes = 0u;
    std::uint64_t allocation_bytes = 0u;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return object != nullptr && owner != nullptr && buffer_handle != 0u &&
               memory_handle != 0u && bytes != 0u && allocation_bytes >= bytes;
    }
    constexpr bool operator==(const runtime_buffer_identity&) const noexcept = default;
};

/// @brief Captures the allocation identity required by operation receipts.
/// @param[in] allocation Runtime-owned allocation to capture.
/// @return The allocation identity, or an empty identity for null input.
[[nodiscard]] inline runtime_buffer_identity
make_runtime_buffer_identity(const buffer* allocation) noexcept {
    if (allocation == nullptr)
        return {};
    return runtime_buffer_identity{
        .object = allocation,
        .owner = allocation->owner,
        .buffer_handle = vulkan_handle_identity(allocation->buffer_handle),
        .memory_handle = vulkan_handle_identity(allocation->memory),
        .bytes = allocation->bytes,
        .allocation_bytes = allocation->allocation_bytes,
    };
}

/// @brief One independent hard chaining range = one dispatched workgroup.
struct gpu_chain_range {
    std::uint32_t begin = 0;
    std::uint32_t length = 0;
};

/// @brief Thrown on any per-run Vulkan failure once the device is usable. Selected
/// Vulkan algorithms must propagate this rather than silently using CPU work.
class vulkan_runtime_error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// @brief Optional supervisor-owned provider for precompiled MoltenVK metallibs.
/// When installed, runtime pipeline creation asks this hook for a shader module
/// instead of invoking glslang/spirv-cross on the backend worker thread.
struct runtime_shader_provider {
    using prepare_function = std::expected<void, std::string> (*)();
    using load_function = std::expected<shader_module_load, std::string> (*)(
        VkDevice device,
        const shader_variant_key& key,
        PFN_vkCreateShaderModule create_shader_module);
    prepare_function prepare_runtime = nullptr;
    load_function load_module = nullptr;
};

/// @brief Returns the process-owned optional shader-provider slot.
/// @return Mutable reference to the process-owned provider pointer.
runtime_shader_provider*& runtime_shader_provider_slot() noexcept {
    static runtime_shader_provider* provider = nullptr;
    return provider;
}

/// @brief Installs the supervisor-provided shader module source.
/// @param[in] provider Optional process-owned provider. Null removes the provider.
void install_runtime_shader_provider(runtime_shader_provider* provider) noexcept {
    runtime_shader_provider_slot() = provider;
}

/// @brief Validates the canonical lowercase hexadecimal runtime-cache digest.
/// @param[in] digest Candidate cache digest.
/// @return True only for a nonzero 16-character lowercase hexadecimal digest.
[[nodiscard]] inline bool is_runtime_cache_digest_valid(std::string_view digest) noexcept {
    return digest.size() == 16u &&
           std::all_of(digest.begin(),
                       digest.end(),
                       [](char value) {
                           return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                       }) &&
           std::any_of(digest.begin(), digest.end(), [](char value) { return value != '0'; });
}

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

    [[nodiscard]] bool is_valid() const noexcept {
        return artifact_ordinal != (std::numeric_limits<std::uint32_t>::max)() &&
               !artifact_filename.empty() && size != 0u && !negative_error_domain.empty() &&
               negative_error_domain.size() <= 1024u &&
               negative_error_domain.find('\0') == std::string::npos &&
               std::any_of(
                   sha256.begin(), sha256.end(), [](std::uint8_t byte) { return byte != 0u; });
    }
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
    [[nodiscard]] bool
    operator==(const runtime_metal_archive_configuration&) const noexcept = default;
};

/// @brief Single admitted process owner for installed archive configuration metadata.
/// The public configuration is a transient installation candidate. Its vector
/// and strings are moved into this image without a second allocation or copy.
class runtime_metal_archive_configuration_image {
  public:
    runtime_metal_archive_configuration_image(const runtime_metal_archive_configuration_image&) =
        delete;
    runtime_metal_archive_configuration_image&
    operator=(const runtime_metal_archive_configuration_image&) = delete;
    runtime_metal_archive_configuration_image(
        runtime_metal_archive_configuration_image&&) noexcept = default;
    runtime_metal_archive_configuration_image&
    operator=(runtime_metal_archive_configuration_image&&) noexcept = delete;

    [[nodiscard]] const runtime_metal_archive_configuration& configuration() const noexcept {
        return configuration_;
    }
    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        return retained_capacity_bytes_;
    }
    [[nodiscard]] std::uint64_t peak_admitted_bytes() const noexcept {
        return peak_admitted_bytes_;
    }
    [[nodiscard]] bool is_valid() const noexcept {
        return configuration_.is_valid() && permit_ && retained_capacity_bytes_ != 0u &&
               retained_capacity_bytes_ <= peak_admitted_bytes_ &&
               permit_.bytes() == retained_capacity_bytes_;
    }

  private:
    runtime_metal_archive_configuration_image(runtime_metal_archive_configuration configuration,
                                              std::uint64_t peak_admitted_bytes,
                                              bioseq::parallelism::memory_permit permit) noexcept
        : permit_(std::move(permit)), configuration_(std::move(configuration)),
          peak_admitted_bytes_(peak_admitted_bytes) {}

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

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return archive_count != 0u && total_size != 0u && ordered_checksum != 0u;
    }
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

    [[nodiscard]] bool is_valid() const noexcept {
        return api_version != 0u && driver_version != 0u &&
               std::any_of(device_uuid.begin(),
                           device_uuid.end(),
                           [](std::uint8_t value) { return value != 0u; }) &&
               std::any_of(pipeline_cache_uuid.begin(),
                           pipeline_cache_uuid.end(),
                           [](std::uint8_t value) { return value != 0u; });
    }
    [[nodiscard]] bool operator==(const runtime_vulkan_device_identity&) const noexcept = default;
};

/// @brief Immutable qualification issued for the process-resident Metal archive set.
struct runtime_metal_archive_process_qualification;
/// @brief Computes the stable identity of a process Metal archive qualification.
/// @return Stable nonzero qualification identity.
[[nodiscard]] std::uint64_t runtime_metal_archive_process_qualification_identity(
    const runtime_metal_archive_process_qualification&) noexcept;

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
    [[nodiscard]] bool
    operator==(const runtime_metal_archive_process_qualification&) const noexcept = default;
};

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
    runtime_metal_archive_pipeline_receipt() = default;
    runtime_metal_archive_pipeline_receipt(const runtime_metal_archive_pipeline_receipt&) = delete;
    runtime_metal_archive_pipeline_receipt&
    operator=(const runtime_metal_archive_pipeline_receipt&) = delete;
    runtime_metal_archive_pipeline_receipt(runtime_metal_archive_pipeline_receipt&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          metrics_claimed_(std::exchange(other.metrics_claimed_, false)) {}
    runtime_metal_archive_pipeline_receipt&
    operator=(runtime_metal_archive_pipeline_receipt&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            metrics_claimed_ = std::exchange(other.metrics_claimed_, false);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return audit_.is_internally_consistent();
    }
    [[nodiscard]] const runtime_metal_archive_pipeline_audit& audit() const noexcept {
        return audit_;
    }

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

inline constexpr std::size_t k_runtime_pipeline_kind_slots = 64u;
inline constexpr std::size_t k_runtime_pipeline_prepared_slots = 11u;

/// @brief Exact 8-bit radix pass count for the persisted grouped-SoA Anchor ref_bits
/// domain: every target-coordinate bit plus the one canonical reverse-strand
/// bit. Kept public and allocation-free so portable contract tests can pin
/// every byte-width boundary used by causal dispatch validation.
/// @return Required byte radix passes, or no value for an invalid span.
[[nodiscard]] inline std::optional<std::uint32_t>
runtime_grouped_soa_radix_pass_count(std::uint64_t target_position_offset,
                                     std::uint64_t target_len) noexcept {
    if (target_len == 0u ||
        target_position_offset > (std::numeric_limits<std::uint64_t>::max)() - (target_len - 1u)) {
        return std::nullopt;
    }
    const std::uint64_t target_end = target_position_offset + target_len - 1u;
    if (target_end >= (std::uint64_t{1} << 63u)) {
        return std::nullopt;
    }
    const std::uint32_t bits =
        target_end == 0u ? 1u : 64u - static_cast<std::uint32_t>(std::countl_zero(target_end));
    return std::clamp<std::uint32_t>((bits + 8u) / 8u, 1u, 8u);
}

/// @brief Semantic operation families tracked by pipeline-use receipts.
enum class runtime_pipeline_operation_kind : std::uint8_t {
    invalid,
    query_sketch,
    grouped_soa_coordinate,
};

/// @brief Exact cache load and pipeline-creation metrics for one operation.
struct runtime_gpu_cache_metrics_audit {
    std::uint64_t token_identity = 0u;
    std::uint64_t gpu_pipeline_cache_loads = 0u;
    std::uint64_t gpu_pipeline_cache_seed_bytes = 0u;
    std::uint64_t gpu_pipeline_cache_load_ns = 0u;
    std::uint64_t gpu_pipeline_creates = 0u;
    std::uint64_t gpu_pipeline_creation_ns = 0u;
    bool saturated = false;

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        const bool seed_shape =
            gpu_pipeline_cache_loads == 0u
                ? gpu_pipeline_cache_seed_bytes == 0u && gpu_pipeline_cache_load_ns == 0u
                : gpu_pipeline_cache_loads == 1u && gpu_pipeline_cache_seed_bytes != 0u &&
                      gpu_pipeline_cache_load_ns != 0u;
        const bool create_shape = gpu_pipeline_creates == 0u ? gpu_pipeline_creation_ns == 0u
                                                             : gpu_pipeline_creation_ns != 0u;
        return token_identity != 0u && !saturated && seed_shape && create_shape;
    }
    [[nodiscard]] constexpr bool
    operator==(const runtime_gpu_cache_metrics_audit&) const noexcept = default;
    [[nodiscard]] constexpr bool
    is_componentwise_bounded_by(const runtime_gpu_cache_metrics_audit& maximum) const noexcept {
        return is_internally_consistent() && maximum.is_internally_consistent() &&
               token_identity == maximum.token_identity &&
               gpu_pipeline_cache_loads <= maximum.gpu_pipeline_cache_loads &&
               gpu_pipeline_cache_seed_bytes <= maximum.gpu_pipeline_cache_seed_bytes &&
               gpu_pipeline_cache_load_ns <= maximum.gpu_pipeline_cache_load_ns &&
               gpu_pipeline_creates <= maximum.gpu_pipeline_creates &&
               gpu_pipeline_creation_ns <= maximum.gpu_pipeline_creation_ns;
    }
};

namespace runtime_detail {

/// @brief Fixed, nonallocating transaction prepared before the final immutable live
/// gate. Construction only observes report-once state. Destruction without
/// commit is a no-op, so a gate failure cannot consume seed/pipeline metrics.
/// runtime holds its queue/cache mutex across prepare, gate and commit.
class runtime_gpu_cache_metrics_claim_plan {
  public:
    runtime_gpu_cache_metrics_claim_plan() = default;
    runtime_gpu_cache_metrics_claim_plan(const runtime_gpu_cache_metrics_claim_plan&) = delete;
    runtime_gpu_cache_metrics_claim_plan&
    operator=(const runtime_gpu_cache_metrics_claim_plan&) = delete;
    runtime_gpu_cache_metrics_claim_plan(runtime_gpu_cache_metrics_claim_plan&& other) noexcept
        : audit_(std::exchange(other.audit_, {})), claims_(std::exchange(other.claims_, {})),
          claim_count_(std::exchange(other.claim_count_, 0u)),
          claim_seed_(std::exchange(other.claim_seed_, false)),
          valid_(std::exchange(other.valid_, false)),
          committed_(std::exchange(other.committed_, false)) {}
    runtime_gpu_cache_metrics_claim_plan&
    operator=(runtime_gpu_cache_metrics_claim_plan&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            claims_ = std::exchange(other.claims_, {});
            claim_count_ = std::exchange(other.claim_count_, 0u);
            claim_seed_ = std::exchange(other.claim_seed_, false);
            valid_ = std::exchange(other.valid_, false);
            committed_ = std::exchange(other.committed_, false);
        }
        return *this;
    }

    [[nodiscard]] static runtime_gpu_cache_metrics_claim_plan
    prepare(std::uint64_t token_identity,
            const runtime_gpu_cache_metrics_audit& maximum,
            bool seed_already_claimed,
            std::span<const runtime_metal_archive_pipeline_receipt* const> receipts) noexcept {
        runtime_gpu_cache_metrics_claim_plan plan{};
        plan.audit_.token_identity = token_identity;
        if (token_identity == 0u || !maximum.is_internally_consistent() ||
            maximum.token_identity != token_identity || receipts.empty() ||
            receipts.size() > plan.claims_.size()) {
            return plan;
        }
        const runtime_metal_archive_pipeline_receipt* first = receipts.front();
        if (first == nullptr || !first->is_valid())
            return plan;
        plan.claim_seed_ = !seed_already_claimed;
        if (plan.claim_seed_) {
            plan.audit_.gpu_pipeline_cache_loads = 1u;
            plan.audit_.gpu_pipeline_cache_seed_bytes = first->audit().seed_size;
            plan.audit_.gpu_pipeline_cache_load_ns = first->audit().seed_load_ns;
        }
        for (std::size_t i = 0u; i < receipts.size(); ++i) {
            const runtime_metal_archive_pipeline_receipt* receipt = receipts[i];
            if (receipt == nullptr || !receipt->is_valid() ||
                receipt->audit().seed_size != first->audit().seed_size ||
                receipt->audit().seed_load_ns != first->audit().seed_load_ns) {
                return runtime_gpu_cache_metrics_claim_plan{};
            }
            for (std::size_t prior = 0u; prior < i; ++prior) {
                if (receipts[prior] == receipt) {
                    return runtime_gpu_cache_metrics_claim_plan{};
                }
            }
            if (receipt->metrics_claimed_)
                continue;
            if (plan.claim_count_ == plan.claims_.size() ||
                plan.audit_.gpu_pipeline_creates == (std::numeric_limits<std::uint64_t>::max)() ||
                receipt->audit().pipeline_create_ns > (std::numeric_limits<std::uint64_t>::max)() -
                                                          plan.audit_.gpu_pipeline_creation_ns) {
                return runtime_gpu_cache_metrics_claim_plan{};
            }
            plan.claims_[plan.claim_count_++] = receipt;
            ++plan.audit_.gpu_pipeline_creates;
            plan.audit_.gpu_pipeline_creation_ns += receipt->audit().pipeline_create_ns;
        }
        plan.valid_ = plan.audit_.is_componentwise_bounded_by(maximum);
        return plan;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return valid_ && audit_.is_internally_consistent();
    }
    [[nodiscard]] bool is_committed() const noexcept {
        return committed_;
    }
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& audit() const noexcept {
        return audit_;
    }

    void commit(bool& seed_claimed) noexcept {
        if (!is_valid() || committed_)
            return;
        if (claim_seed_)
            seed_claimed = true;
        for (std::size_t i = 0u; i < claim_count_; ++i) {
            claims_[i]->metrics_claimed_ = true;
        }
        committed_ = true;
    }

  private:
    runtime_gpu_cache_metrics_audit audit_{};
    std::array<const runtime_metal_archive_pipeline_receipt*, k_runtime_pipeline_prepared_slots>
        claims_{};
    std::size_t claim_count_ = 0u;
    bool claim_seed_ = false;
    bool valid_ = false;
    bool committed_ = false;
};

static_assert(!std::is_copy_constructible_v<runtime_gpu_cache_metrics_claim_plan>);
static_assert(std::is_nothrow_move_constructible_v<runtime_gpu_cache_metrics_claim_plan>);

} // namespace runtime_detail

/// @brief Move-only metrics delta that may be consumed exactly once.
class runtime_gpu_cache_metrics_delta {
  public:
    runtime_gpu_cache_metrics_delta() = default;
    runtime_gpu_cache_metrics_delta(const runtime_gpu_cache_metrics_delta&) = delete;
    runtime_gpu_cache_metrics_delta& operator=(const runtime_gpu_cache_metrics_delta&) = delete;
    runtime_gpu_cache_metrics_delta(runtime_gpu_cache_metrics_delta&& other) noexcept
        : audit_(std::exchange(other.audit_, {})), issued_(std::exchange(other.issued_, false)) {}
    runtime_gpu_cache_metrics_delta& operator=(runtime_gpu_cache_metrics_delta&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            issued_ = std::exchange(other.issued_, false);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && audit_.is_internally_consistent();
    }
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& audit() const noexcept {
        return audit_;
    }
    void consume() noexcept {
        DEBUG_ASSERT(is_valid(), "persisted GPU cache metrics token was not prevalidated");
        audit_ = {};
        issued_ = false;
    }

  private:
    runtime_gpu_cache_metrics_audit audit_{};
    bool issued_ = false;
    friend class runtime;
};

static_assert(!std::is_copy_constructible_v<runtime_gpu_cache_metrics_delta>);
static_assert(std::is_nothrow_move_constructible_v<runtime_gpu_cache_metrics_delta>);

/// @brief Accepted immutable pipeline-cache policy for GPU operations.
enum class runtime_pipeline_cache_policy : std::uint8_t {
    invalid,
    immutable_preseed_all_hit,
};

/// @brief Immutable GPU environment identity attached to accepted operations.
struct runtime_gpu_execution_environment_snapshot {
    bool gpu_environment_present = false;
    std::array<std::uint8_t, VK_UUID_SIZE> gpu_device_uuid{};
    std::array<char, 128> gpu_driver_identity{};
    runtime_pipeline_cache_policy cache_policy = runtime_pipeline_cache_policy::invalid;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    bool pipeline_cache_all_hit = false;

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return gpu_environment_present &&
               std::any_of(gpu_device_uuid.begin(),
                           gpu_device_uuid.end(),
                           [](std::uint8_t value) { return value != 0u; }) &&
               std::any_of(gpu_driver_identity.begin(),
                           gpu_driver_identity.end(),
                           [](char value) { return value != '\0'; }) &&
               cache_policy == runtime_pipeline_cache_policy::immutable_preseed_all_hit &&
               std::any_of(seed_sha256.begin(),
                           seed_sha256.end(),
                           [](std::uint8_t value) { return value != 0u; }) &&
               seed_size != 0u && manifest_checksum != 0u && pipeline_cache_all_hit;
    }
    [[nodiscard]] constexpr bool
    operator==(const runtime_gpu_execution_environment_snapshot&) const noexcept = default;
};

/// @brief Preparation receipt for one shader pipeline identity.
struct runtime_pipeline_prepared_audit {
    shader_variant_key shader_key{};
    std::uint64_t receipt_identity = 0u;
    std::uint64_t prepared_calls = 0u;
};

/// @brief Per-job dispatch, workgroup, and submission accounting.
struct runtime_pipeline_job_use_audit {
    std::array<std::uint64_t, k_runtime_pipeline_kind_slots> dispatches{};
    std::array<std::uint64_t, k_runtime_pipeline_kind_slots> workgroups{};
    std::uint64_t submission_operation_identity = 0u;
    std::array<std::uint64_t, 4> submission_ordinals{};
    std::uint64_t committed_submissions = 0u;
    std::uint64_t minimizer_count = 0u;
    std::uint64_t target_len = 0u;
    std::uint64_t target_position_offset = 0u;
    std::uint64_t anchor_count = 0u;
    std::uint64_t range_count = 0u;
    bool shape_contract_set = false;
    bool saturated = false;

    [[nodiscard]] std::uint64_t count(shader_kind kind) const noexcept {
        const std::size_t slot = static_cast<std::size_t>(kind);
        return slot < dispatches.size() ? dispatches[slot] : 0u;
    }
};

/// @brief Complete causal audit for one runtime pipeline operation.
struct runtime_pipeline_operation_audit {
    runtime_pipeline_operation_kind kind = runtime_pipeline_operation_kind::invalid;
    std::uint64_t operation_identity = 0u;
    std::array<runtime_pipeline_prepared_audit, k_runtime_pipeline_prepared_slots> prepared{};
    std::size_t prepared_count = 0u;
    std::vector<runtime_pipeline_job_use_audit> jobs{};
    std::uint64_t committed_submissions = 0u;
    std::uint64_t begun_command_generations = 0u;
    std::uint64_t committed_command_generations = 0u;
    std::uint64_t burned_pending_uses = 0u;
    bool pending = false;
    bool invalid = false;
    bool saturated = false;
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    std::uint64_t seed_receipt_identity = 0u;
    runtime_metal_archive_set_identity archive_set{};
    runtime_metal_archive_process_qualification cache_qualification{};
    runtime_gpu_cache_metrics_audit cache_metrics{};
};

/// @brief Collects pipeline preparation and committed dispatch use for one operation.
class runtime_pipeline_use_collector;
/// @brief Move-only receipt for a completed grouped-SoA coordinate operation.
class persisted_grouped_soa_coordinate_operation_receipt;

/// @brief Move-only causal receipt that binds PREPARED cache receipts to dispatches
/// committed only after submit, fence wait, immutable live gate, and reset.
class runtime_pipeline_operation_token {
  public:
    runtime_pipeline_operation_token() = default;
    runtime_pipeline_operation_token(const runtime_pipeline_operation_token&) = delete;
    runtime_pipeline_operation_token& operator=(const runtime_pipeline_operation_token&) = delete;
    runtime_pipeline_operation_token(runtime_pipeline_operation_token&& other) noexcept
        : capacity_permit_(std::move(other.capacity_permit_)), audit_(std::move(other.audit_)),
          receipts_(std::exchange(other.receipts_, {})),
          issued_(std::exchange(other.issued_, false)) {
        other.audit_ = {};
    }
    runtime_pipeline_operation_token& operator=(runtime_pipeline_operation_token&& other) noexcept {
        if (this != &other) {
            // Vector move-assignment releases the prior admitted allocation
            // while its permit is still live. Only then may the prior permit
            // be replaced by the source permit.
            audit_ = std::move(other.audit_);
            capacity_permit_ = std::move(other.capacity_permit_);
            receipts_ = std::exchange(other.receipts_, {});
            issued_ = std::exchange(other.issued_, false);
            other.audit_ = {};
        }
        return *this;
    }

    [[nodiscard]] const runtime_pipeline_operation_audit& audit() const noexcept {
        return audit_;
    }
    [[nodiscard]] bool is_valid_for_query_sketch() const noexcept;
    [[nodiscard]] bool is_valid_for_grouped_soa_coordinate() const noexcept;
    [[nodiscard]] static bool
    self_test_causal_collector_contract(runtime_metal_archive_pipeline_audit base_audit);

  private:
    [[nodiscard]] bool is_common_state_valid() const noexcept;
    [[nodiscard]] std::uint64_t prepared_calls(shader_kind kind) const noexcept;
    [[nodiscard]] bool is_prepared_exactly(std::span<const shader_kind> required,
                                           std::uint64_t radix_prepared_calls = 0u) const noexcept;

    // Declared before the vector owner so destruction deallocates jobs before
    // releasing their reservation (members are destroyed in reverse order).
    bioseq::parallelism::memory_permit capacity_permit_{};
    runtime_pipeline_operation_audit audit_{};
    std::array<const runtime_metal_archive_pipeline_receipt*, k_runtime_pipeline_prepared_slots>
        receipts_{};
    bool issued_ = false;
    friend class runtime;
    friend class runtime_pipeline_use_collector;
    friend class persisted_grouped_soa_coordinate_operation_receipt;
};

static_assert(!std::is_copy_constructible_v<runtime_pipeline_operation_token>);
static_assert(std::is_nothrow_move_constructible_v<runtime_pipeline_operation_token>);

inline bool runtime_pipeline_operation_token::is_common_state_valid() const noexcept {
    if (!issued_ || audit_.operation_identity == 0u ||
        audit_.kind == runtime_pipeline_operation_kind::invalid || audit_.jobs.empty() ||
        audit_.prepared_count == 0u || audit_.prepared_count > audit_.prepared.size() ||
        audit_.committed_submissions == 0u ||
        audit_.committed_command_generations != audit_.committed_submissions ||
        audit_.begun_command_generations != audit_.committed_command_generations ||
        audit_.burned_pending_uses != 0u || audit_.pending || audit_.invalid || audit_.saturated ||
        !audit_.cache_metrics.is_internally_consistent() ||
        audit_.cache_metrics.token_identity != audit_.operation_identity ||
        audit_.api_version == 0u || audit_.driver_version == 0u || audit_.seed_size == 0u ||
        audit_.manifest_checksum == 0u || audit_.seed_receipt_identity == 0u ||
        !audit_.archive_set.is_valid() || !audit_.cache_qualification.is_internally_consistent() ||
        audit_.cache_qualification.archive_set != audit_.archive_set ||
        std::all_of(audit_.device_uuid.begin(),
                    audit_.device_uuid.end(),
                    [](std::uint8_t value) { return value == 0u; }) ||
        std::all_of(audit_.pipeline_cache_uuid.begin(),
                    audit_.pipeline_cache_uuid.end(),
                    [](std::uint8_t value) { return value == 0u; }) ||
        std::all_of(audit_.seed_sha256.begin(), audit_.seed_sha256.end(), [](std::uint8_t value) {
            return value == 0u;
        })) {
        return false;
    }
    for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
        const runtime_pipeline_prepared_audit& prepared = audit_.prepared[i];
        const runtime_metal_archive_pipeline_receipt* receipt = receipts_[i];
        if (receipt == nullptr || !receipt->is_valid() || prepared.prepared_calls == 0u ||
            prepared.receipt_identity == 0u || !is_shader_variant_key_valid(prepared.shader_key)) {
            return false;
        }
        const runtime_metal_archive_pipeline_audit& pipeline = receipt->audit();
        if (pipeline.receipt_identity != prepared.receipt_identity ||
            pipeline.shader_key != prepared.shader_key ||
            pipeline.runtime_vulkan_device_uuid != audit_.device_uuid ||
            pipeline.api_version != audit_.api_version ||
            pipeline.driver_version != audit_.driver_version ||
            pipeline.vendor_id != audit_.vendor_id || pipeline.device_id != audit_.device_id ||
            pipeline.pipeline_cache_uuid != audit_.pipeline_cache_uuid ||
            pipeline.seed_sha256 != audit_.seed_sha256 || pipeline.seed_size != audit_.seed_size ||
            pipeline.manifest_checksum != audit_.manifest_checksum ||
            pipeline.seed_receipt_identity != audit_.seed_receipt_identity ||
            pipeline.expected_archives != audit_.archive_set ||
            pipeline.cache_qualification != audit_.cache_qualification) {
            return false;
        }
    }
    for (const runtime_pipeline_job_use_audit& job : audit_.jobs) {
        if (job.saturated || job.committed_submissions == 0u)
            return false;
        if (job.submission_operation_identity != audit_.operation_identity ||
            job.committed_submissions > job.submission_ordinals.size()) {
            return false;
        }
        for (std::size_t i = 0u; i < job.submission_ordinals.size(); ++i) {
            if (i < job.committed_submissions) {
                if (job.submission_ordinals[i] == 0u)
                    return false;
                for (std::size_t prior = 0u; prior < i; ++prior) {
                    if (job.submission_ordinals[prior] == job.submission_ordinals[i])
                        return false;
                }
            } else if (job.submission_ordinals[i] != 0u) {
                return false;
            }
        }
        for (std::size_t kind = 0u; kind < job.dispatches.size(); ++kind) {
            if ((job.dispatches[kind] == 0u) != (job.workgroups[kind] == 0u) ||
                (job.dispatches[kind] != 0u && job.workgroups[kind] < job.dispatches[kind])) {
                return false;
            }
        }
    }
    return true;
}

inline std::uint64_t
runtime_pipeline_operation_token::prepared_calls(shader_kind kind) const noexcept {
    const std::uint32_t raw = static_cast<std::uint32_t>(kind);
    for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
        if (audit_.prepared[i].shader_key.kind == raw) {
            return audit_.prepared[i].prepared_calls;
        }
    }
    return 0u;
}

inline bool runtime_pipeline_operation_token::is_prepared_exactly(
    std::span<const shader_kind> required, std::uint64_t radix_prepared_calls) const noexcept {
    const std::size_t expected_count = required.size() + (radix_prepared_calls == 0u ? 0u : 3u);
    if (audit_.prepared_count != expected_count)
        return false;
    for (const shader_kind kind : required) {
        if (prepared_calls(kind) != 1u)
            return false;
    }
    const std::array radix{
        shader_kind::radix_sort_histogram,
        shader_kind::radix_sort_scan,
        shader_kind::radix_sort_scatter,
    };
    for (const shader_kind kind : radix) {
        if (prepared_calls(kind) != radix_prepared_calls)
            return false;
    }
    for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
        const shader_kind actual = static_cast<shader_kind>(audit_.prepared[i].shader_key.kind);
        const bool base = std::find(required.begin(), required.end(), actual) != required.end();
        const bool sort = std::find(radix.begin(), radix.end(), actual) != radix.end();
        if (!base && !(radix_prepared_calls != 0u && sort))
            return false;
    }
    return true;
}

inline bool runtime_pipeline_operation_token::is_valid_for_query_sketch() const noexcept {
    constexpr std::array required{
        shader_kind::ordered_sketch_tile,
        shader_kind::prefix_scan_block,
        shader_kind::prefix_scan_add,
    };
    if (!is_common_state_valid() || audit_.kind != runtime_pipeline_operation_kind::query_sketch ||
        audit_.jobs.size() != 1u || audit_.committed_submissions != 2u ||
        !is_prepared_exactly(required)) {
        return false;
    }
    const runtime_pipeline_job_use_audit& job = audit_.jobs.front();
    if (job.committed_submissions != 2u || job.count(shader_kind::ordered_sketch_tile) != 2u ||
        job.count(shader_kind::prefix_scan_block) != 1u ||
        job.count(shader_kind::prefix_scan_add) != 2u) {
        return false;
    }
    std::uint64_t total = 0u;
    for (const std::uint64_t count : job.dispatches) {
        if (count > (std::numeric_limits<std::uint64_t>::max)() - total) {
            return false;
        }
        total += count;
    }
    return total == 5u;
}

inline bool runtime_pipeline_operation_token::is_valid_for_grouped_soa_coordinate() const noexcept {
    constexpr std::array required{
        shader_kind::grouped_soa_seed_count,
        shader_kind::grouped_soa_seed_emit,
        shader_kind::prefix_scan_block,
        shader_kind::prefix_scan_add,
        shader_kind::chain_range_build,
        shader_kind::coordinate_reduce,
        shader_kind::chain_tiled_dp_u64,
    };
    if (!is_common_state_valid() ||
        audit_.kind != runtime_pipeline_operation_kind::grouped_soa_coordinate) {
        return false;
    }
    std::uint64_t anchored_jobs = 0u;
    for (const runtime_pipeline_job_use_audit& job : audit_.jobs) {
        if (!job.shape_contract_set || job.minimizer_count == 0u || job.target_len == 0u ||
            job.target_position_offset >
                (std::numeric_limits<std::uint64_t>::max)() - (job.target_len - 1u)) {
            return false;
        }
        const auto pass_count =
            runtime_grouped_soa_radix_pass_count(job.target_position_offset, job.target_len);
        if (!pass_count.has_value())
            return false;
        const std::uint64_t passes = *pass_count;
        const bool front = job.count(shader_kind::grouped_soa_seed_count) == 1u &&
                           job.count(shader_kind::prefix_scan_block) == 1u &&
                           job.count(shader_kind::prefix_scan_add) == 2u;
        if (!front)
            return false;
        std::uint64_t expected_total = 4u;
        std::uint64_t expected_submissions = 1u;
        if (job.anchor_count == 0u) {
            if (job.range_count != 0u)
                return false;
        } else {
            ++anchored_jobs;
            expected_total = 6u + 3u * passes;
            expected_submissions = 3u;
            if (job.count(shader_kind::grouped_soa_seed_emit) != 1u ||
                job.count(shader_kind::radix_sort_histogram) != passes ||
                job.count(shader_kind::radix_sort_scan) != passes ||
                job.count(shader_kind::radix_sort_scatter) != passes ||
                job.count(shader_kind::chain_range_build) != 1u) {
                return false;
            }
            if (job.range_count != 0u) {
                expected_total += 2u;
                expected_submissions = 4u;
                if (job.count(shader_kind::chain_tiled_dp_u64) != 1u ||
                    job.count(shader_kind::coordinate_reduce) != 1u) {
                    return false;
                }
            }
        }
        std::uint64_t actual_total = 0u;
        for (const std::uint64_t count : job.dispatches) {
            if (count > (std::numeric_limits<std::uint64_t>::max)() - actual_total) {
                return false;
            }
            actual_total += count;
        }
        if (actual_total != expected_total || job.committed_submissions != expected_submissions ||
            job.count(shader_kind::coordinate_vote_reduce) != 0u ||
            job.count(shader_kind::coordinate_vote_reduce_batch) != 0u ||
            job.count(shader_kind::ordered_sketch_tile) != 0u) {
            return false;
        }
    }
    return is_prepared_exactly(required, anchored_jobs);
}

/// @brief Mutable collector used while recording and committing GPU submissions.
class runtime_pipeline_use_collector {
  public:
    runtime_pipeline_use_collector(const runtime_pipeline_use_collector&) = delete;
    runtime_pipeline_use_collector& operator=(const runtime_pipeline_use_collector&) = delete;
    runtime_pipeline_use_collector(runtime_pipeline_use_collector&&) = delete;
    runtime_pipeline_use_collector& operator=(runtime_pipeline_use_collector&&) = delete;
    ~runtime_pipeline_use_collector() {
        burn_pending();
    }

  private:
    struct pending_job {
        std::array<std::uint64_t, k_runtime_pipeline_kind_slots> dispatches{};
        std::array<std::uint64_t, k_runtime_pipeline_kind_slots> workgroups{};
    };

    [[nodiscard]] static std::unique_ptr<runtime_pipeline_use_collector>
    create(runtime_pipeline_operation_kind kind,
           std::size_t job_count,
           std::uint64_t operation_identity) {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (kind == runtime_pipeline_operation_kind::invalid || job_count == 0u ||
            operation_identity == 0u ||
            job_count > std::vector<runtime_pipeline_job_use_audit>{}.max_size() ||
            job_count > std::vector<pending_job>{}.max_size()) {
            throw vulkan_runtime_error("runtime pipeline receipt collector shape is invalid");
        }
        std::uint64_t retained = sizeof(runtime_pipeline_use_collector);
        retained = saturating_add(retained, 2u * 1024u);
        retained = saturating_add(retained,
                                  saturating_mul(static_cast<std::uint64_t>(job_count),
                                                 sizeof(runtime_pipeline_job_use_audit)));
        retained = saturating_add(
            retained, saturating_mul(static_cast<std::uint64_t>(job_count), sizeof(pending_job)));
        const std::uint64_t peak =
            saturating_mul(retained, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw vulkan_runtime_error("runtime pipeline receipt collector admission saturated");
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            peak, &failure);
        if (!permit) {
            throw vulkan_runtime_error(shader_cache_message(
                "runtime pipeline receipt collector admission denied: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                " requested=",
                failure.requested_bytes));
        }
        std::unique_ptr<runtime_pipeline_use_collector> result;
        try {
            result.reset(new runtime_pipeline_use_collector(
                kind, job_count, operation_identity, std::move(*permit)));
        } catch (const std::bad_alloc&) {
            throw vulkan_runtime_error(
                "runtime pipeline receipt collector allocation failed after admission");
        }
        result->reconcile_capacity_permit();
        return result;
    }

    runtime_pipeline_use_collector(runtime_pipeline_operation_kind kind,
                                   std::size_t job_count,
                                   std::uint64_t operation_identity,
                                   bioseq::parallelism::memory_permit capacity_permit)
        : capacity_permit_(std::move(capacity_permit)), pending_(job_count) {
        audit_.kind = kind;
        audit_.operation_identity = operation_identity;
        audit_.jobs.resize(job_count);
        audit_.cache_metrics.token_identity = operation_identity;
        submission_ordinal_cursor_ = 0u;
    }

    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t retained = sizeof(runtime_pipeline_use_collector);
        retained = saturating_add(retained, 2u * 1024u);
        retained = saturating_add(retained,
                                  saturating_mul(static_cast<std::uint64_t>(audit_.jobs.capacity()),
                                                 sizeof(runtime_pipeline_job_use_audit)));
        retained = saturating_add(
            retained,
            saturating_mul(static_cast<std::uint64_t>(pending_.capacity()), sizeof(pending_job)));
        return retained;
    }

    void reconcile_capacity_permit() {
        const std::uint64_t retained = retained_capacity_bytes();
        if (retained == (std::numeric_limits<std::uint64_t>::max)() || !capacity_permit_ ||
            retained > capacity_permit_.bytes()) {
            throw vulkan_runtime_error(
                "runtime pipeline receipt collector exceeded admitted capacity");
        }
        capacity_permit_.shrink_to(retained);
    }

    static bool add_counter(std::uint64_t& destination, std::uint64_t value) noexcept {
        if (value > (std::numeric_limits<std::uint64_t>::max)() - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            return false;
        }
        destination += value;
        return true;
    }

    void invalidate() noexcept {
        audit_.invalid = true;
    }

    void prepare(const shader_variant_key& key,
                 const runtime_metal_archive_pipeline_receipt* receipt) noexcept {
        if (audit_.invalid || receipt == nullptr || !receipt->is_valid() ||
            receipt->audit().shader_key != key || receipt->audit().pipeline_create_ns == 0u) {
            invalidate();
            return;
        }
        std::size_t slot = audit_.prepared_count;
        for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
            if (audit_.prepared[i].shader_key == key) {
                slot = i;
                break;
            }
        }
        if (slot == audit_.prepared_count) {
            if (slot == audit_.prepared.size()) {
                invalidate();
                return;
            }
            audit_.prepared[slot] = runtime_pipeline_prepared_audit{
                .shader_key = key,
                .receipt_identity = receipt->audit().receipt_identity,
                .prepared_calls = 0u,
            };
            receipts_[slot] = receipt;
            ++audit_.prepared_count;
        } else if (receipts_[slot] != receipt ||
                   audit_.prepared[slot].receipt_identity != receipt->audit().receipt_identity) {
            invalidate();
            return;
        }
        if (!add_counter(audit_.prepared[slot].prepared_calls, 1u)) {
            audit_.saturated = true;
            return;
        }

        const runtime_metal_archive_pipeline_audit& source = receipt->audit();
        if (!environment_sealed_) {
            audit_.device_uuid = source.runtime_vulkan_device_uuid;
            audit_.api_version = source.api_version;
            audit_.driver_version = source.driver_version;
            audit_.vendor_id = source.vendor_id;
            audit_.device_id = source.device_id;
            audit_.pipeline_cache_uuid = source.pipeline_cache_uuid;
            audit_.seed_sha256 = source.seed_sha256;
            audit_.seed_size = source.seed_size;
            audit_.manifest_checksum = source.manifest_checksum;
            audit_.seed_receipt_identity = source.seed_receipt_identity;
            audit_.archive_set = source.expected_archives;
            audit_.cache_qualification = source.cache_qualification;
            environment_sealed_ = true;
        } else if (source.runtime_vulkan_device_uuid != audit_.device_uuid ||
                   source.api_version != audit_.api_version ||
                   source.driver_version != audit_.driver_version ||
                   source.vendor_id != audit_.vendor_id || source.device_id != audit_.device_id ||
                   source.pipeline_cache_uuid != audit_.pipeline_cache_uuid ||
                   source.seed_sha256 != audit_.seed_sha256 ||
                   source.seed_size != audit_.seed_size ||
                   source.manifest_checksum != audit_.manifest_checksum ||
                   source.seed_receipt_identity != audit_.seed_receipt_identity ||
                   source.expected_archives != audit_.archive_set ||
                   source.cache_qualification != audit_.cache_qualification) {
            invalidate();
            return;
        }
        // Report-once counters are deliberately not claimed during PREPARE.
        // A command that later resets, burns, or fails causal validation must
        // not steal metrics from the next wholly accepted operation. The
        // finalizer claims the canonical seed/receipt bits transactionally
        // under runtime::mu_ after the immutable live gate succeeds.
    }

    void set_shape_input(std::size_t job_index,
                         std::uint64_t minimizer_count,
                         std::uint64_t target_len,
                         std::uint64_t target_position_offset) noexcept {
        if (job_index >= audit_.jobs.size()) {
            invalidate();
            return;
        }
        runtime_pipeline_job_use_audit& job = audit_.jobs[job_index];
        job.minimizer_count = minimizer_count;
        job.target_len = target_len;
        job.target_position_offset = target_position_offset;
        job.shape_contract_set = true;
    }

    void set_shape_outcome(std::size_t job_index,
                           std::uint64_t anchor_count,
                           std::uint64_t range_count) noexcept {
        if (job_index >= audit_.jobs.size() || !audit_.jobs[job_index].shape_contract_set) {
            invalidate();
            return;
        }
        audit_.jobs[job_index].anchor_count = anchor_count;
        audit_.jobs[job_index].range_count = range_count;
    }

    void begin(VkCommandBuffer command_buffer) noexcept {
        if (active_) {
            burn_pending();
            invalidate();
        }
        if (command_buffer == VK_NULL_HANDLE || audit_.invalid) {
            invalidate();
            return;
        }
        active_ = true;
        command_identity_ = vulkan_handle_identity(command_buffer);
        if (command_identity_ == 0u || !add_counter(audit_.begun_command_generations, 1u)) {
            audit_.saturated = true;
            invalidate();
            return;
        }
        active_generation_ = audit_.begun_command_generations;
    }

    void use(VkCommandBuffer command_buffer,
             std::size_t job_index,
             const shader_variant_key& key,
             const runtime_metal_archive_pipeline_receipt* receipt,
             std::uint32_t group_count,
             std::uint64_t observed_generation = 0u) noexcept {
        if (!active_ || audit_.invalid || job_index >= pending_.size() ||
            vulkan_handle_identity(command_buffer) != command_identity_ || group_count == 0u ||
            (observed_generation != 0u && observed_generation != active_generation_) ||
            receipt == nullptr || !receipt->is_valid()) {
            invalidate();
            return;
        }
        bool prepared = false;
        for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
            if (audit_.prepared[i].shader_key == key && receipts_[i] == receipt &&
                audit_.prepared[i].receipt_identity == receipt->audit().receipt_identity) {
                prepared = true;
                break;
            }
        }
        const std::size_t kind = key.kind;
        if (!prepared || kind >= k_runtime_pipeline_kind_slots ||
            !add_counter(pending_[job_index].dispatches[kind], 1u) ||
            !add_counter(pending_[job_index].workgroups[kind], group_count)) {
            audit_.jobs[job_index].saturated = true;
            audit_.saturated = true;
            invalidate();
            return;
        }
        audit_.pending = true;
    }

  public:
    void commit(VkCommandBuffer command_buffer) noexcept {
        if (!active_ || vulkan_handle_identity(command_buffer) != command_identity_) {
            burn_pending();
            invalidate();
            return;
        }
        bool any = false;
        if (submission_ordinal_cursor_ == (std::numeric_limits<std::uint64_t>::max)()) {
            burn_pending();
            audit_.saturated = true;
            invalidate();
            return;
        }
        const std::uint64_t submission_ordinal = ++submission_ordinal_cursor_;
        for (std::size_t job_index = 0u; job_index < pending_.size(); ++job_index) {
            bool touched = false;
            for (std::size_t kind = 0u; kind < k_runtime_pipeline_kind_slots; ++kind) {
                const std::uint64_t count = pending_[job_index].dispatches[kind];
                if (count == 0u)
                    continue;
                touched = true;
                any = true;
                if (!add_counter(audit_.jobs[job_index].dispatches[kind], count)) {
                    audit_.jobs[job_index].saturated = true;
                    audit_.saturated = true;
                }
                if (!add_counter(audit_.jobs[job_index].workgroups[kind],
                                 pending_[job_index].workgroups[kind])) {
                    audit_.jobs[job_index].saturated = true;
                    audit_.saturated = true;
                }
                pending_[job_index].dispatches[kind] = 0u;
                pending_[job_index].workgroups[kind] = 0u;
            }
            if (touched) {
                runtime_pipeline_job_use_audit& job = audit_.jobs[job_index];
                if (job.committed_submissions >= job.submission_ordinals.size() ||
                    submission_ordinal == 0u ||
                    (job.submission_operation_identity != 0u &&
                     job.submission_operation_identity != audit_.operation_identity)) {
                    job.saturated = true;
                    audit_.saturated = true;
                    invalidate();
                } else {
                    job.submission_operation_identity = audit_.operation_identity;
                    job.submission_ordinals[job.committed_submissions] = submission_ordinal;
                    if (!add_counter(job.committed_submissions, 1u)) {
                        job.saturated = true;
                        audit_.saturated = true;
                    }
                }
            }
        }
        if (!any || !add_counter(audit_.committed_submissions, 1u) ||
            !add_counter(audit_.committed_command_generations, 1u)) {
            invalidate();
        }
        active_ = false;
        command_identity_ = 0u;
        active_generation_ = 0u;
        audit_.pending = false;
    }

    void burn_pending() noexcept {
        if (!active_)
            return;
        for (auto& job : pending_) {
            for (std::size_t kind = 0u; kind < job.dispatches.size(); ++kind) {
                std::uint64_t& count = job.dispatches[kind];
                if (count != 0u && !add_counter(audit_.burned_pending_uses, count)) {
                    audit_.saturated = true;
                }
                count = 0u;
                job.workgroups[kind] = 0u;
            }
        }
        active_ = false;
        command_identity_ = 0u;
        active_generation_ = 0u;
        audit_.pending = false;
    }

    void burn_before_reset(VkCommandBuffer command_buffer) noexcept {
        if (!active_)
            return;
        if (vulkan_handle_identity(command_buffer) != command_identity_) {
            invalidate();
        }
        burn_pending();
        // A begun generation that reaches reset without a causal commit is
        // never an accepted empty command.  This also rejects the no-use case
        // where burned_pending_uses alone would remain zero.
        invalidate();
    }

  private:
    [[nodiscard]] runtime_pipeline_operation_token seal() noexcept {
        if (active_) {
            audit_.pending = true;
            burn_pending();
            invalidate();
        }
        runtime_pipeline_operation_token receipt{};
        receipt.audit_ = std::move(audit_);
        receipt.receipts_ = receipts_;
        receipt.issued_ = true;
        // Pending storage no longer contributes to the sealed receipt. Destroy
        // it before shrinking/transferring the permit. Keep the collector
        // object itself covered until its unique_ptr owner destroys it; the
        // resulting small over-reservation then travels with the jobs vector
        // for the complete lifetime of the move-only receipt.
        {
            std::vector<pending_job> empty;
            pending_.swap(empty);
        }
        {
            // The standard allocator transfers this capacity, but explicitly
            // clear any valid moved-from residue before the permit leaves the
            // collector so the ownership receipt is allocator-independent.
            std::vector<runtime_pipeline_job_use_audit> empty;
            audit_.jobs.swap(empty);
        }
        const std::uint64_t retained_jobs = bioseq::common::saturating_add(
            sizeof(runtime_pipeline_use_collector),
            bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(receipt.audit_.jobs.capacity()),
                sizeof(runtime_pipeline_job_use_audit)));
        if (retained_jobs == (std::numeric_limits<std::uint64_t>::max)() ||
            retained_jobs > capacity_permit_.bytes()) {
            receipt.audit_.invalid = true;
        } else {
            capacity_permit_.shrink_to(retained_jobs);
        }
        receipt.capacity_permit_ = std::move(capacity_permit_);
        receipts_.fill(nullptr);
        audit_ = {};
        return receipt;
    }

    // Kept before both vector owners: they release capacity before the permit.
    bioseq::parallelism::memory_permit capacity_permit_{};
    runtime_pipeline_operation_audit audit_{};
    std::array<const runtime_metal_archive_pipeline_receipt*, k_runtime_pipeline_prepared_slots>
        receipts_{};
    std::vector<pending_job> pending_{};
    std::uint64_t command_identity_ = 0u;
    std::uint64_t active_generation_ = 0u;
    std::uint64_t submission_ordinal_cursor_ = 0u;
    bool environment_sealed_ = false;
    bool active_ = false;

    friend class runtime;
    friend class runtime_pipeline_operation_token;
};

inline bool runtime_pipeline_operation_token::self_test_causal_collector_contract(
    runtime_metal_archive_pipeline_audit base_audit) {
    if (!base_audit.is_internally_consistent() || base_audit.pipeline_create_ns == 0u) {
        return false;
    }
    const std::array<shader_variant_key, 11> keys{
        make_shader_key(shader_kind::ordered_sketch_tile, 384u, 64u, 10u),
        make_shader_key(shader_kind::prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(shader_kind::prefix_scan_add, 384u, 128u, 1u),
        make_shader_key(shader_kind::grouped_soa_seed_count, 384u, 128u, 3u),
        make_shader_key(shader_kind::grouped_soa_seed_emit, 384u, 128u, 3u),
        make_shader_key(shader_kind::chain_range_build, 384u, 1u, 1u),
        make_shader_key(shader_kind::coordinate_reduce, 384u, 128u, 1u),
        make_shader_key(shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u),
        make_shader_key(shader_kind::radix_sort_histogram, 384u, 1u, 1u),
        make_shader_key(shader_kind::radix_sort_scan, 384u, 1u, 1u),
        make_shader_key(shader_kind::radix_sort_scatter, 384u, 1u, 1u),
    };
    if (!std::all_of(keys.begin(), keys.end(), [](const shader_variant_key& key) {
            return is_shader_variant_key_valid(key);
        })) {
        return false;
    }
    std::array<runtime_metal_archive_pipeline_receipt, 11> receipts{};
    bool seed_claimed = false;
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        runtime_metal_archive_pipeline_audit audit = base_audit;
        audit.receipt_identity = 1000u + i;
        audit.scope_identity = 2000u + i;
        audit.shader_key = keys[i];
        receipts[i] = runtime_metal_archive_pipeline_receipt{std::move(audit)};
        if (!receipts[i].is_valid())
            return false;
    }
    const auto command = [](std::uintptr_t identity) {
        return reinterpret_cast<VkCommandBuffer>(identity);
    };
    const auto prepare_query = [&keys, &receipts](runtime_pipeline_use_collector& collector) {
        for (std::size_t i = 0u; i < 3u; ++i) {
            collector.prepare(keys[i], &receipts[i]);
        }
    };
    const auto query_golden = [&keys, &prepare_query, &receipts, command]() {
        auto collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::query_sketch, 1u, 101u);
        prepare_query(*collector);
        collector->begin(command(1u));
        collector->use(command(1u), 0u, keys[0], &receipts[0], 3u);
        collector->use(command(1u), 0u, keys[1], &receipts[1], 2u);
        collector->use(command(1u), 0u, keys[2], &receipts[2], 2u);
        collector->commit(command(1u));
        collector->begin(command(2u));
        collector->use(command(2u), 0u, keys[0], &receipts[0], 3u);
        collector->use(command(2u), 0u, keys[2], &receipts[2], 1u);
        collector->commit(command(2u));
        return collector->seal();
    };

    runtime_pipeline_operation_token query{};
    try {
        query = query_golden();
    } catch (...) {
        return false;
    }
    if (!query.is_valid_for_query_sketch() || query.audit_.begun_command_generations != 2u ||
        query.audit_.committed_command_generations != 2u ||
        query.audit_.jobs[0].submission_operation_identity != 101u ||
        query.audit_.jobs[0].submission_ordinals[0] != 1u ||
        query.audit_.jobs[0].submission_ordinals[1] != 2u ||
        query.audit_.jobs[0].count(shader_kind::ordered_sketch_tile) != 2u ||
        query.audit_.jobs[0]
                .workgroups[static_cast<std::size_t>(shader_kind::ordered_sketch_tile)] != 6u ||
        query.audit_.jobs[0].workgroups[static_cast<std::size_t>(shader_kind::prefix_scan_block)] !=
            2u ||
        query.audit_.jobs[0].workgroups[static_cast<std::size_t>(shader_kind::prefix_scan_add)] !=
            3u ||
        !query.capacity_permit_) {
        return false;
    }
    runtime_pipeline_operation_token moved_query{std::move(query)};
    if (query.is_valid_for_query_sketch() || query.capacity_permit_ ||
        !moved_query.is_valid_for_query_sketch() || !moved_query.capacity_permit_) {
        return false;
    }
    query = std::move(moved_query);
    if (moved_query.is_valid_for_query_sketch() || moved_query.capacity_permit_ ||
        !query.is_valid_for_query_sketch()) {
        return false;
    }

    const auto rejected_query = [&keys, &prepare_query, &receipts, command](std::uint64_t identity,
                                                                            int mode) {
        auto collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::query_sketch, 1u, identity);
        prepare_query(*collector);
        const VkCommandBuffer begun = command(static_cast<std::uintptr_t>(identity));
        collector->begin(begun);
        if (mode != 1) {
            collector->use(mode == 4 ? command(static_cast<std::uintptr_t>(identity + 1000u))
                                     : begun,
                           0u,
                           keys[0],
                           mode == 5 ? &receipts[1] : &receipts[0],
                           mode == 6 ? 0u : 1u,
                           mode == 2 ? 2u : 0u);
        }
        if (mode <= 2 || mode >= 4) {
            collector->burn_before_reset(begun);
        }
        return collector->seal();
    };
    try {
        const runtime_pipeline_operation_token reset = rejected_query(102u, 0);
        const runtime_pipeline_operation_token empty = rejected_query(103u, 1);
        const runtime_pipeline_operation_token stale = rejected_query(104u, 2);
        const runtime_pipeline_operation_token uncommitted = rejected_query(105u, 3);
        const runtime_pipeline_operation_token wrong_command = rejected_query(106u, 4);
        const runtime_pipeline_operation_token wrong_receipt = rejected_query(107u, 5);
        const runtime_pipeline_operation_token zero_groups = rejected_query(108u, 6);
        if (reset.is_valid_for_query_sketch() || reset.audit_.burned_pending_uses != 1u ||
            empty.is_valid_for_query_sketch() || empty.audit_.begun_command_generations != 1u ||
            stale.is_valid_for_query_sketch() || uncommitted.is_valid_for_query_sketch() ||
            wrong_command.is_valid_for_query_sketch() ||
            wrong_receipt.is_valid_for_query_sketch() || zero_groups.is_valid_for_query_sketch()) {
            return false;
        }
    } catch (...) {
        return false;
    }

    runtime_pipeline_operation_token shape{};
    try {
        auto collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::grouped_soa_coordinate, 3u, 202u);
        constexpr std::array<std::size_t, 7> base_slots{3u, 4u, 1u, 2u, 5u, 6u, 7u};
        for (const std::size_t slot : base_slots) {
            collector->prepare(keys[slot], &receipts[slot]);
        }
        // One radix pipeline retrieval per anchored job, independent of the
        // number of byte passes recorded in that job's command.
        for (std::size_t repeat = 0u; repeat < 2u; ++repeat) {
            for (std::size_t slot = 8u; slot < 11u; ++slot) {
                collector->prepare(keys[slot], &receipts[slot]);
            }
        }
        collector->set_shape_input(0u, 1u, 1u, 0u);
        collector->set_shape_input(1u, 1u, 255u, 0u);
        collector->set_shape_input(2u, 1u, 65536u, 0u);
        collector->set_shape_outcome(0u, 0u, 0u);
        collector->set_shape_outcome(1u, 1u, 0u);
        collector->set_shape_outcome(2u, 1u, 1u);

        collector->begin(command(11u));
        for (std::size_t job = 0u; job < 3u; ++job) {
            collector->use(command(11u), job, keys[3], &receipts[3], 1u);
            collector->use(command(11u), job, keys[1], &receipts[1], 1u);
            collector->use(command(11u), job, keys[2], &receipts[2], 1u);
            collector->use(command(11u), job, keys[2], &receipts[2], 1u);
        }
        collector->commit(command(11u));

        collector->begin(command(12u));
        for (std::size_t job = 1u; job < 3u; ++job) {
            collector->use(command(12u), job, keys[4], &receipts[4], 1u);
        }
        collector->commit(command(12u));

        collector->begin(command(13u));
        for (std::size_t slot = 8u; slot < 11u; ++slot) {
            collector->use(command(13u), 1u, keys[slot], &receipts[slot], 1u);
            collector->use(command(13u), 1u, keys[slot], &receipts[slot], 1u);
            collector->use(command(13u), 2u, keys[slot], &receipts[slot], 1u);
            collector->use(command(13u), 2u, keys[slot], &receipts[slot], 1u);
            collector->use(command(13u), 2u, keys[slot], &receipts[slot], 1u);
        }
        collector->use(command(13u), 1u, keys[5], &receipts[5], 1u);
        collector->use(command(13u), 2u, keys[5], &receipts[5], 1u);
        collector->commit(command(13u));

        collector->begin(command(14u));
        collector->use(command(14u), 2u, keys[7], &receipts[7], 1u);
        collector->use(command(14u), 2u, keys[6], &receipts[6], 1u);
        collector->commit(command(14u));
        shape = collector->seal();
    } catch (...) {
        return false;
    }
    if (!shape.is_valid_for_grouped_soa_coordinate() ||
        shape.audit_.begun_command_generations != 4u ||
        shape.audit_.committed_command_generations != 4u || !shape.capacity_permit_) {
        return false;
    }
    constexpr std::array<std::uint64_t, 3> expected_submissions{1u, 3u, 4u};
    for (std::size_t job = 0u; job < shape.audit_.jobs.size(); ++job) {
        const runtime_pipeline_job_use_audit& audit = shape.audit_.jobs[job];
        if (audit.submission_operation_identity != 202u ||
            audit.committed_submissions != expected_submissions[job] ||
            audit.count(shader_kind::coordinate_vote_reduce) != 0u ||
            audit.count(shader_kind::coordinate_vote_reduce_batch) != 0u) {
            return false;
        }
        for (std::size_t ordinal = 0u; ordinal < audit.committed_submissions; ++ordinal) {
            if (audit.submission_ordinals[ordinal] != ordinal + 1u) {
                return false;
            }
        }
        for (std::size_t kind = 0u; kind < audit.dispatches.size(); ++kind) {
            if (audit.dispatches[kind] != audit.workgroups[kind]) {
                return false;
            }
        }
    }
    if (query.audit_.jobs[0].submission_ordinals[0] !=
            shape.audit_.jobs[0].submission_ordinals[0] ||
        query.audit_.jobs[0].submission_operation_identity ==
            shape.audit_.jobs[0].submission_operation_identity) {
        return false;
    }

    // PREPARE, reset/burn and sealing are descriptive only. Exercise the exact
    // fixed transaction used by the production finalizer: abandoning a plan
    // models a live-gate failure; the following accepted commit reports all
    // counters, and the next accepted operation reports zero.
    if (std::any_of(receipts.begin(),
                    receipts.end(),
                    [](const auto& receipt) { return receipt.metrics_claimed_; }) ||
        seed_claimed) {
        return false;
    }
    runtime_gpu_cache_metrics_audit maximum{
        .token_identity = 303u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = receipts[0].audit().seed_size,
        .gpu_pipeline_cache_load_ns = receipts[0].audit().seed_load_ns,
        .gpu_pipeline_creates = static_cast<std::uint64_t>(receipts.size()),
    };
    for (const runtime_metal_archive_pipeline_receipt& receipt : receipts) {
        if (receipt.audit().pipeline_create_ns >
            (std::numeric_limits<std::uint64_t>::max)() - maximum.gpu_pipeline_creation_ns) {
            return false;
        }
        maximum.gpu_pipeline_creation_ns += receipt.audit().pipeline_create_ns;
    }
    std::array<const runtime_metal_archive_pipeline_receipt*, 11> receipt_pointers{};
    for (std::size_t i = 0u; i < receipts.size(); ++i) {
        receipt_pointers[i] = &receipts[i];
    }
    const auto pointers = std::span<const runtime_metal_archive_pipeline_receipt* const>{
        receipt_pointers.data(), receipt_pointers.size()};
    {
        auto abandoned = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
            maximum.token_identity, maximum, seed_claimed, pointers);
        if (!abandoned.is_valid() || abandoned.audit() != maximum || abandoned.is_committed()) {
            return false;
        }
    }
    if (seed_claimed || std::any_of(receipts.begin(), receipts.end(), [](const auto& receipt) {
            return receipt.metrics_claimed_;
        })) {
        return false;
    }
    auto first = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        maximum.token_identity, maximum, seed_claimed, pointers);
    if (!first.is_valid() || first.audit() != maximum)
        return false;
    first.commit(seed_claimed);
    if (!first.is_committed() || !seed_claimed ||
        std::any_of(receipts.begin(), receipts.end(), [](const auto& receipt) {
            return !receipt.metrics_claimed_;
        })) {
        return false;
    }
    runtime_gpu_cache_metrics_audit later_maximum = maximum;
    later_maximum.token_identity = 304u;
    auto later = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        later_maximum.token_identity, later_maximum, seed_claimed, pointers);
    if (!later.is_valid() || later.audit().token_identity != later_maximum.token_identity ||
        later.audit().gpu_pipeline_cache_loads != 0u ||
        later.audit().gpu_pipeline_cache_seed_bytes != 0u ||
        later.audit().gpu_pipeline_cache_load_ns != 0u ||
        later.audit().gpu_pipeline_creates != 0u || later.audit().gpu_pipeline_creation_ns != 0u) {
        return false;
    }
    later.commit(seed_claimed);
    return later.is_committed() && seed_claimed;
}

/// @brief Install-once production seam.  The supervisor calls this from its runtime
/// prepare hook, before the first runtime singleton construction.
/// @return Success or a configuration identity or admission diagnostic.
[[nodiscard]] std::expected<void, std::string> install_runtime_metal_archive_configuration(
    std::shared_ptr<const runtime_metal_archive_configuration_image> configuration);

[[nodiscard]] std::expected<std::shared_ptr<const runtime_metal_archive_configuration_image>,
                            std::string>
make_runtime_metal_archive_configuration_image(
    std::shared_ptr<const sealed_shader_cache_seed_image> seed_image,
    std::array<std::uint8_t, VK_UUID_SIZE> vulkan_device_uuid = {},
    std::uint64_t metal_device_registry_id = 0u);

[[nodiscard]] bool is_runtime_metal_archive_configuration_installed() noexcept;

} // namespace bioseq::gpu::vulkan

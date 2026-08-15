/**
 * @file pipeline.cppm
 * @brief Defines causal pipeline audits, cache metrics, tokens, and collectors.
 */
module;

#include <cstddef>
#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:receipts.pipeline;

import :receipts.core;
import :receipts.metal_archive;
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

} // namespace bioseq::gpu::vulkan

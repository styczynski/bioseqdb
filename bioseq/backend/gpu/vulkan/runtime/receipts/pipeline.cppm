/**
 * @file pipeline.cppm
 * @brief Defines causal pipeline audits, cache metrics, tokens, and collectors.
 */
module;

#include <cstddef>
#include <cstdint>
#include <libassert/assert.hpp>
#include <new>

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

/// Fixed shader-kind counter capacity in each per-job audit.
inline constexpr std::size_t k_runtime_pipeline_kind_slots = 64u;
/// Maximum distinct prepared pipeline identities retained by one token.
inline constexpr std::size_t k_runtime_pipeline_prepared_slots = 16u;

/// @brief Exact 8-bit radix pass count for the persisted grouped-SoA Anchor ref_bits
/// domain: every target-coordinate bit plus the one canonical reverse-strand
/// bit. Kept public and allocation-free so portable contract tests can pin
/// every byte-width boundary used by causal dispatch validation.
/// @param[in] target_position_offset Global target-coordinate offset.
/// @param[in] target_len Target span in bases.
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
    invalid,                   ///< Unissued or moved-from operation.
    query_sketch,              ///< Query sketch count, scan, and emit operation.
    grouped_soa_coordinate,    ///< Direct persisted seed-to-coordinate operation.
    persisted_alignment_cigar, ///< Packed-window GPU alignment and CIGAR operation.
    persisted_mapq,            ///< Post-alignment GPU classification and MAPQ operation.
    source_direct_coordinate,  ///< Source-direct seed, chain, and coordinate operation.
    grouped_soa_seed_anchor_roundtrip, ///< MPS count/scan/emit/sort-only operation.
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    chaining_tiled_dp,          ///< Selected tiled-DP score/predecessor submission only.
    mpf_seed_anchor_roundtrip,  ///< Existing MPF seed emission/count and GPU sort work.
#endif
};

/// @brief Exact cache load and pipeline-creation metrics for one operation.
struct runtime_gpu_cache_metrics_audit {
    std::uint64_t token_identity = 0u;
    std::uint64_t gpu_pipeline_cache_loads = 0u;
    std::uint64_t gpu_pipeline_cache_seed_bytes = 0u;
    std::uint64_t gpu_pipeline_cache_load_ns = 0u;
    std::uint64_t gpu_pipeline_creates = 0u;
    std::uint64_t gpu_pipeline_creation_ns = 0u;
    /// Set when a counter increment overflowed and the audit was marked unusable.
    bool saturated = false;

    /// @brief Checks token identity plus seed-load and pipeline-create metric shapes.
    /// @return Whether all counters form a finite internally consistent audit.
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

    /// @brief Compares every cache-metric audit field.
    [[nodiscard]] constexpr bool
    operator==(const runtime_gpu_cache_metrics_audit&) const noexcept = default;

    /// @brief Checks whether this delta fits within a matching maximum audit.
    /// @param[in] maximum Maximum audit for the same operation token.
    /// @return Whether identities match and every metric is componentwise bounded.
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
    /// @brief Creates an empty invalid claim plan.
    runtime_gpu_cache_metrics_claim_plan() = default;

    /// @brief Prevents copying one-shot metric claims.
    runtime_gpu_cache_metrics_claim_plan(const runtime_gpu_cache_metrics_claim_plan&) = delete;

    /// @brief Prevents copy assignment of one-shot metric claims.
    runtime_gpu_cache_metrics_claim_plan&
    operator=(const runtime_gpu_cache_metrics_claim_plan&) = delete;

    /// @brief Transfers the pending audit and claim set.
    /// @param[in,out] other Claim plan whose authority is consumed.
    runtime_gpu_cache_metrics_claim_plan(runtime_gpu_cache_metrics_claim_plan&& other) noexcept
        : audit_(std::exchange(other.audit_, {})), claims_(std::exchange(other.claims_, {})),
          claim_count_(std::exchange(other.claim_count_, 0u)),
          claim_seed_(std::exchange(other.claim_seed_, false)),
          valid_(std::exchange(other.valid_, false)),
          committed_(std::exchange(other.committed_, false)) {}

    /// @brief Replaces this plan by transferring another pending claim set.
    /// @param[in,out] other Claim plan whose authority is consumed.
    /// @return This claim plan.
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

    /// @brief Builds a nonmutating plan for the unclaimed seed and receipt metrics.
    /// @param[in] token_identity Operation token identity shared by all metrics.
    /// @param[in] maximum Maximum accepted metrics for the operation.
    /// @param[in] seed_already_claimed Whether the process seed metrics were already reported.
    /// @param[in] receipts Distinct pipeline receipts eligible for one-shot claims.
    /// @return Valid claim plan, or an empty invalid plan when any contract fails.
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
        plan.claim_seed_ = !first->audit().native_vulkan && !seed_already_claimed;
        if (plan.claim_seed_) {
            plan.audit_.gpu_pipeline_cache_loads = 1u;
            plan.audit_.gpu_pipeline_cache_seed_bytes = first->audit().seed_size;
            plan.audit_.gpu_pipeline_cache_load_ns = first->audit().seed_load_ns;
        }
        for (std::size_t i = 0u; i < receipts.size(); ++i) {
            const runtime_metal_archive_pipeline_receipt* receipt = receipts[i];
            if (receipt == nullptr || !receipt->is_valid() ||
                receipt->audit().native_vulkan != first->audit().native_vulkan ||
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

    /// @brief Checks whether the prepared audit is finite and accepted.
    /// @return Whether the plan may be committed.
    [[nodiscard]] bool is_valid() const noexcept {
        return valid_ && audit_.is_internally_consistent();
    }

    /// @brief Reports whether this plan already consumed its claims.
    /// @return Whether `commit` completed once.
    [[nodiscard]] bool is_committed() const noexcept {
        return committed_;
    }

    /// @brief Returns the prepared cache-metric delta.
    /// @return Borrowed audit view valid for this plan's lifetime.
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& audit() const noexcept {
        return audit_;
    }

    /// @brief Atomically marks the planned seed and pipeline metrics as claimed.
    /// @param[in,out] seed_claimed Process-wide seed-metric claim flag.
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
    /// @brief Creates an empty invalid metrics delta.
    runtime_gpu_cache_metrics_delta() = default;

    /// @brief Prevents copying one-shot metrics authority.
    runtime_gpu_cache_metrics_delta(const runtime_gpu_cache_metrics_delta&) = delete;

    /// @brief Prevents copy assignment of one-shot metrics authority.
    runtime_gpu_cache_metrics_delta& operator=(const runtime_gpu_cache_metrics_delta&) = delete;

    /// @brief Transfers a one-shot metrics delta.
    /// @param[in,out] other Delta whose audit and issued state are consumed.
    runtime_gpu_cache_metrics_delta(runtime_gpu_cache_metrics_delta&& other) noexcept
        : audit_(std::exchange(other.audit_, {})), issued_(std::exchange(other.issued_, false)) {}

    /// @brief Replaces this delta by transferring another delta's authority.
    /// @param[in,out] other Delta whose audit and issued state are consumed.
    /// @return This metrics delta.
    runtime_gpu_cache_metrics_delta& operator=(runtime_gpu_cache_metrics_delta&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            issued_ = std::exchange(other.issued_, false);
        }
        return *this;
    }

    /// @brief Checks issued state and metric consistency.
    /// @return Whether this delta may be consumed once.
    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && audit_.is_internally_consistent();
    }

    /// @brief Returns the immutable metrics carried by this delta.
    /// @return Borrowed audit view valid until consumption or movement.
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& audit() const noexcept {
        return audit_;
    }

    /// @brief Consumes the one-shot delta and clears its audit.
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
    invalid,                   ///< No accepted cache policy.
    immutable_preseed_all_hit, ///< Every pipeline came from the sealed preseed set.
    native_vulkan,             ///< Native dispatch; Metal cache qualification is inapplicable.
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

    /// @brief Checks device, driver, cache policy, seed identity, and all-hit state.
    /// @return Whether this snapshot represents an accepted immutable GPU environment.
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        if (cache_policy == runtime_pipeline_cache_policy::native_vulkan) {
            return k_native_vulkan_pipeline_receipts && gpu_environment_present &&
                   std::any_of(gpu_device_uuid.begin(), gpu_device_uuid.end(),
                               [](std::uint8_t value) { return value != 0u; }) &&
                   std::any_of(gpu_driver_identity.begin(), gpu_driver_identity.end(),
                               [](char value) { return value != '\0'; }) &&
                   seed_sha256 == std::array<std::uint8_t, 32>{} &&
                   seed_size == 0u && manifest_checksum == 0u && !pipeline_cache_all_hit;
        }
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

    /// @brief Compares every GPU environment snapshot field.
    [[nodiscard]] constexpr bool
    operator==(const runtime_gpu_execution_environment_snapshot&) const noexcept = default;
};

/// @brief Preparation receipt for one shader pipeline identity.
struct runtime_pipeline_prepared_audit {
    shader_variant_key shader_key{};
    std::uint64_t receipt_identity = 0u;
    /// Successful `prepare` call count for this shader key within the operation.
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
    std::uint32_t binding_window_count = 0u; ///< Required count/emit dispatches for MPS.
    std::uint64_t query_authority_identity = 0u;
    std::uint64_t anchor_digest_lo = k_seed_anchor_digest_seed_low;
    std::uint64_t anchor_digest_hi = k_seed_anchor_digest_seed_high;
    bool anchor_order_checked = false;
    bool anchor_order_accepted = false;
    /// Set after `set_shape_input` records minimizer and target dimensions for this job.
    bool shape_contract_set = false;
    /// Set when a per-job counter increment overflowed.
    bool saturated = false;

    /// @brief Returns the committed dispatch count for one shader kind.
    /// @param[in] kind Shader kind whose dispatch slot is queried.
    /// @return Recorded dispatch count, or zero for an out-of-range kind.
    [[nodiscard]] std::uint64_t count(shader_kind kind) const noexcept {
        const std::size_t slot = static_cast<std::size_t>(kind);
        return slot < dispatches.size() ? dispatches[slot] : 0u;
    }
};

/// @brief Complete causal audit for one runtime pipeline operation.
struct runtime_pipeline_operation_audit {
    bool native_vulkan = false;
    runtime_pipeline_operation_kind kind = runtime_pipeline_operation_kind::invalid;
    std::uint64_t operation_identity = 0u;
    std::array<runtime_pipeline_prepared_audit, k_runtime_pipeline_prepared_slots> prepared{};
    std::size_t prepared_count = 0u;
    std::vector<runtime_pipeline_job_use_audit> jobs{};
    std::uint64_t committed_submissions = 0u;
    std::uint64_t begun_command_generations = 0u;
    std::uint64_t committed_command_generations = 0u;
    /// Dispatches recorded in a burned generation before causal commit.
    std::uint64_t burned_pending_uses = 0u;
    /// Uncommitted dispatches remain in an active command generation.
    bool pending = false;
    /// Collector or seal path recorded a contract violation.
    bool invalid = false;
    /// Set when an operation-level counter increment overflowed.
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
    /// @brief Creates an empty unissued operation token.
    runtime_pipeline_operation_token() = default;

    /// @brief Prevents copying causal receipt authority.
    runtime_pipeline_operation_token(const runtime_pipeline_operation_token&) = delete;

    /// @brief Prevents copy assignment of causal receipt authority.
    runtime_pipeline_operation_token& operator=(const runtime_pipeline_operation_token&) = delete;

    /// @brief Transfers the admission permit, audit, receipt links, and issued state.
    /// @param[in,out] other Token whose causal authority is consumed.
    runtime_pipeline_operation_token(runtime_pipeline_operation_token&& other) noexcept
        : capacity_permit_(std::move(other.capacity_permit_)), audit_(std::move(other.audit_)),
          receipts_(std::exchange(other.receipts_, {})),
          issued_(std::exchange(other.issued_, false)) {
        other.audit_ = {};
    }

    /// @brief Replaces this token by transferring another token's causal authority.
    /// @param[in,out] other Token whose causal authority is consumed.
    /// @return This operation token.
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

    /// @brief Returns the complete causal operation audit.
    /// @return Borrowed audit view valid for this token's lifetime.
    [[nodiscard]] const runtime_pipeline_operation_audit& audit() const noexcept {
        return audit_;
    }
    /// @brief Checks the causal receipt for one query-sketch operation.
    /// @return Whether every required preparation, dispatch, and submission is present.
    [[nodiscard]] bool is_valid_for_query_sketch() const noexcept {
        constexpr std::array required{
            shader_kind::ordered_sketch_tile,
            shader_kind::prefix_scan_block,
            shader_kind::prefix_scan_add,
        };
        if (!is_common_state_valid() ||
            audit_.kind != runtime_pipeline_operation_kind::query_sketch ||
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

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    /// Validate exactly one score/predecessor dispatch, never coordinate reduction.
    [[nodiscard]] bool is_valid_for_chaining_tiled_dp() const noexcept {
        constexpr std::array required{shader_kind::chain_tiled_dp_u64};
        if (!is_common_state_valid() ||
            audit_.kind != runtime_pipeline_operation_kind::chaining_tiled_dp ||
            audit_.jobs.size() != 1u || audit_.committed_submissions != 1u ||
            !is_prepared_exactly(required)) {
            return false;
        }
        const auto& job = audit_.jobs.front();
        if (!job.shape_contract_set || job.minimizer_count != 0u ||
            job.target_len != 0u || job.target_position_offset != 0u ||
            job.anchor_count == 0u || job.range_count == 0u ||
            job.range_count > job.anchor_count || job.committed_submissions != 1u ||
            job.count(shader_kind::chain_tiled_dp_u64) != 1u) {
            return false;
        }
        for (std::size_t kind = 0u; kind < job.dispatches.size(); ++kind) {
            if (kind != static_cast<std::size_t>(shader_kind::chain_tiled_dp_u64) &&
                job.dispatches[kind] != 0u) {
                return false;
            }
        }
        return true;
    }

    /// Bind score-only dispatch geometry to the live device's X workgroup limit.
    [[nodiscard]] bool is_valid_for_chaining_tiled_dp(
        std::uint32_t max_workgroup_count_x) const noexcept {
        if (max_workgroup_count_x == 0u || !is_valid_for_chaining_tiled_dp()) {
            return false;
        }
        const auto& job = audit_.jobs.front();
        return job.workgroups[static_cast<std::size_t>(shader_kind::chain_tiled_dp_u64)] ==
               std::min(job.range_count, static_cast<std::uint64_t>(max_workgroup_count_x));
    }

    /// Validate either existing MPF frontend from its actual pipeline uses.
    /// Prefix-driven empty jobs have no dispatch token; fused zero-hit jobs
    /// retain their real count/scan submission and never claim an emit or sort.
    [[nodiscard]] bool is_valid_for_mpf_seed_anchor_roundtrip(
        std::uint32_t max_workgroup_count_x) const noexcept {
        if (max_workgroup_count_x == 0u || !is_common_state_valid() ||
            audit_.kind != runtime_pipeline_operation_kind::mpf_seed_anchor_roundtrip ||
            audit_.jobs.size() != 1u) {
            return false;
        }
        const auto& job = audit_.jobs.front();
        constexpr std::uint64_t first_invalid_reference = std::uint64_t{1u} << 63u;
        if (!job.shape_contract_set || job.minimizer_count == 0u ||
            job.minimizer_count > (std::numeric_limits<std::uint32_t>::max)() ||
            job.target_len == 0u || job.target_position_offset >= first_invalid_reference ||
            job.target_len > first_invalid_reference - job.target_position_offset ||
            job.anchor_count > (std::numeric_limits<std::uint32_t>::max)() ||
            job.range_count != 0u || job.query_authority_identity == 0u ||
            !job.anchor_order_checked || !job.anchor_order_accepted) {
            return false;
        }
        const auto count = [&job](shader_kind kind) noexcept { return job.count(kind); };
        const bool prefix_driven = count(shader_kind::seed_lookup_anchors) != 0u;
        const bool bitonic = count(shader_kind::sort_anchors_bitonic) != 0u;
        std::uint64_t expected_dispatches = 0u;
        std::uint64_t expected_submissions = 0u;
        const std::uint64_t seed_groups = (job.minimizer_count + 127u) / 128u;
        if (prefix_driven) {
            if (job.anchor_count == 0u || count(shader_kind::seed_lookup_anchors) != 1u ||
                seed_groups > max_workgroup_count_x ||
                job.workgroups[static_cast<std::size_t>(shader_kind::seed_lookup_anchors)] !=
                    seed_groups) {
                return false;
            }
            expected_dispatches = 1u;
            expected_submissions = 1u;
            if (bitonic) {
                constexpr std::array required{
                    shader_kind::seed_lookup_anchors, shader_kind::sort_anchors_bitonic};
                if (!is_prepared_exactly(required) ||
                    count(shader_kind::sort_anchors_bitonic) != 1u ||
                    job.workgroups[static_cast<std::size_t>(shader_kind::sort_anchors_bitonic)] != 1u) {
                    return false;
                }
                ++expected_dispatches;
            } else {
                constexpr std::array required{shader_kind::seed_lookup_anchors};
                if (!is_prepared_exactly(required, 1u)) {
                    return false;
                }
            }
        } else {
            constexpr std::array required{
                shader_kind::seed_count, shader_kind::seed_emit,
                shader_kind::prefix_scan_block, shader_kind::prefix_scan_add};
            if (bitonic || !is_prepared_exactly(required, job.anchor_count != 0u ? 1u : 0u) ||
                count(shader_kind::seed_count) != 1u ||
                count(shader_kind::prefix_scan_block) != 1u ||
                count(shader_kind::prefix_scan_add) != 2u ||
                count(shader_kind::seed_emit) != (job.anchor_count != 0u ? 1u : 0u) ||
                job.workgroups[static_cast<std::size_t>(shader_kind::seed_count)] !=
                    std::min(seed_groups, static_cast<std::uint64_t>(max_workgroup_count_x))) {
                return false;
            }
            expected_dispatches = 4u + (job.anchor_count != 0u ? 1u : 0u);
            expected_submissions = job.anchor_count != 0u ? 2u : 1u;
            if (job.anchor_count == 0u &&
                (job.anchor_digest_lo != k_seed_anchor_digest_seed_low ||
                 job.anchor_digest_hi != k_seed_anchor_digest_seed_high)) {
                return false;
            }
        }
        if (job.anchor_count != 0u && !bitonic) {
            const auto passes = runtime_grouped_soa_radix_pass_count(
                job.target_position_offset, job.target_len);
            if (!passes || count(shader_kind::radix_sort_histogram) != *passes ||
                count(shader_kind::radix_sort_scan) != *passes ||
                count(shader_kind::radix_sort_scatter) != *passes) {
                return false;
            }
            expected_dispatches += 3u * *passes;
        }
        std::uint64_t actual_dispatches = 0u;
        for (const auto dispatches : job.dispatches) {
            if (dispatches > (std::numeric_limits<std::uint64_t>::max)() - actual_dispatches) {
                return false;
            }
            actual_dispatches += dispatches;
        }
        return actual_dispatches == expected_dispatches &&
               job.committed_submissions == expected_submissions &&
               audit_.committed_submissions == expected_submissions;
    }
#endif

    /// @brief Checks a completed MPS seed-only count/scan/emit/sort receipt.
    [[nodiscard]] bool is_valid_for_grouped_soa_seed_anchor_roundtrip() const noexcept {
        constexpr std::array required{
            shader_kind::grouped_soa_seed_count,
            shader_kind::grouped_soa_seed_emit,
            shader_kind::prefix_scan_block,
            shader_kind::prefix_scan_add,
        };
        if (!is_common_state_valid(true) ||
            audit_.kind !=
                runtime_pipeline_operation_kind::grouped_soa_seed_anchor_roundtrip) {
            return false;
        }

        std::uint64_t common_authority = 0u;
        std::uint64_t anchored_jobs = 0u;
        std::uint64_t active_jobs = 0u;
        for (const runtime_pipeline_job_use_audit& job : audit_.jobs) {
            if (!job.shape_contract_set || job.binding_window_count == 0u || job.target_len == 0u ||
                job.target_position_offset >
                    (std::numeric_limits<std::uint64_t>::max)() - (job.target_len - 1u) ||
                job.range_count != 0u || job.query_authority_identity == 0u ||
                !job.anchor_order_checked || !job.anchor_order_accepted) {
                return false;
            }
            if (common_authority == 0u) {
                common_authority = job.query_authority_identity;
            } else if (job.query_authority_identity != common_authority) {
                return false;
            }
            const auto pass_count =
                runtime_grouped_soa_radix_pass_count(job.target_position_offset, job.target_len);
            if (!pass_count.has_value())
                return false;

            std::uint64_t actual_total = 0u;
            for (const std::uint64_t count : job.dispatches) {
                if (count > (std::numeric_limits<std::uint64_t>::max)() - actual_total)
                    return false;
                actual_total += count;
            }
            if (job.minimizer_count == 0u) {
                if (job.anchor_count != 0u || job.committed_submissions != 0u ||
                    actual_total != 0u || job.anchor_digest_lo != k_seed_anchor_digest_seed_low ||
                    job.anchor_digest_hi != k_seed_anchor_digest_seed_high) {
                    return false;
                }
                continue;
            }

            ++active_jobs;
            std::uint64_t expected_total = std::uint64_t{job.binding_window_count} + 3u;
            if (job.count(shader_kind::grouped_soa_seed_count) != job.binding_window_count ||
                job.count(shader_kind::prefix_scan_block) != 1u ||
                job.count(shader_kind::prefix_scan_add) != 2u) {
                return false;
            }
            if (job.anchor_count == 0u) {
                if (job.count(shader_kind::grouped_soa_seed_emit) != 0u ||
                    job.committed_submissions != 1u ||
                    job.anchor_digest_lo != k_seed_anchor_digest_seed_low ||
                    job.anchor_digest_hi != k_seed_anchor_digest_seed_high) {
                    return false;
                }
            } else {
                ++anchored_jobs;
                const std::uint64_t passes = *pass_count;
                expected_total += job.binding_window_count + 3u * passes;
                if (job.count(shader_kind::grouped_soa_seed_emit) != job.binding_window_count ||
                    job.committed_submissions != 2u ||
                    job.count(shader_kind::radix_sort_histogram) != passes ||
                    job.count(shader_kind::radix_sort_scan) != passes ||
                    job.count(shader_kind::radix_sort_scatter) != passes) {
                    return false;
                }
            }
            if (actual_total != expected_total)
                return false;
        }
        if (active_jobs == 0u && audit_.committed_submissions != 0u)
            return false;
        if (active_jobs != 0u && audit_.committed_submissions == 0u)
            return false;
        return is_prepared_exactly(required, anchored_jobs);
    }

    /// @brief Checks the causal receipt for a grouped-SoA coordinate operation.
    /// @return Whether every required job, pass, dispatch, and submission is present.
    [[nodiscard]] bool is_valid_for_grouped_soa_coordinate() const noexcept {
        constexpr std::array legacy_required{
            shader_kind::grouped_soa_seed_count,
            shader_kind::grouped_soa_seed_emit,
            shader_kind::prefix_scan_block,
            shader_kind::prefix_scan_add,
            shader_kind::chain_range_build,
            shader_kind::coordinate_reduce,
            shader_kind::chain_tiled_dp_u64,
        };
        constexpr std::array candidate_required{
            shader_kind::grouped_soa_seed_count,
            shader_kind::grouped_soa_seed_emit,
            shader_kind::prefix_scan_block,
            shader_kind::prefix_scan_add,
            shader_kind::chain_range_build,
            shader_kind::chain_candidate_reduce,
            shader_kind::chain_tiled_dp_u64,
        };
        if (!is_common_state_valid() ||
            audit_.kind != runtime_pipeline_operation_kind::grouped_soa_coordinate) {
            return false;
        }
        const bool candidate_mode = prepared_calls(shader_kind::chain_candidate_reduce) == 1u;
        if ((prepared_calls(shader_kind::coordinate_reduce) == 1u) == candidate_mode) {
            return false;
        }
        std::uint64_t anchored_jobs = 0u;
        for (const runtime_pipeline_job_use_audit& job : audit_.jobs) {
            if (!job.shape_contract_set || job.binding_window_count == 0u || job.minimizer_count == 0u || job.target_len == 0u ||
                job.target_position_offset >
                    (std::numeric_limits<std::uint64_t>::max)() - (job.target_len - 1u)) {
                return false;
            }
            const auto pass_count =
                runtime_grouped_soa_radix_pass_count(job.target_position_offset, job.target_len);
            if (!pass_count.has_value())
                return false;
            const std::uint64_t passes = *pass_count;
            const bool front = job.count(shader_kind::grouped_soa_seed_count) == job.binding_window_count &&
                               job.count(shader_kind::prefix_scan_block) == 1u &&
                               job.count(shader_kind::prefix_scan_add) == 2u;
            if (!front)
                return false;
            std::uint64_t expected_total = std::uint64_t{job.binding_window_count} + 3u;
            std::uint64_t expected_submissions = 1u;
            if (job.anchor_count == 0u) {
                if (job.range_count != 0u)
                    return false;
            } else {
                ++anchored_jobs;
                expected_total = 2ull * job.binding_window_count + 4u + 3u * passes;
                expected_submissions = 3u;
                if (job.count(shader_kind::grouped_soa_seed_emit) != job.binding_window_count ||
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
                        job.count(candidate_mode ? shader_kind::chain_candidate_reduce
                                                 : shader_kind::coordinate_reduce) != 1u ||
                        job.count(candidate_mode ? shader_kind::coordinate_reduce
                                                 : shader_kind::chain_candidate_reduce) != 0u) {
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
            if (actual_total != expected_total ||
                job.committed_submissions != expected_submissions ||
                job.count(shader_kind::coordinate_vote_reduce) != 0u ||
                job.count(shader_kind::coordinate_vote_reduce_batch) != 0u ||
                job.count(shader_kind::ordered_sketch_tile) != 0u) {
                return false;
            }
        }
        return is_prepared_exactly(candidate_mode ? std::span<const shader_kind>{candidate_required}
                                                  : std::span<const shader_kind>{legacy_required},
                                   anchored_jobs);
    }

    /// @brief Checks one persisted packed-window alignment/CIGAR logical operation.
    /// @return Whether every job has one normal dispatch or one probe plus exact retry.
    [[nodiscard]] bool is_valid_for_persisted_alignment_cigar() const noexcept {
        if (!is_common_state_valid() ||
            audit_.kind != runtime_pipeline_operation_kind::persisted_alignment_cigar ||
            audit_.prepared_count != 1u ||
            prepared_calls(shader_kind::align_extend_simple_cigar) !=
                audit_.committed_submissions) {
            return false;
        }
        std::uint64_t prior_problem_ordinal = 0u;
        std::uint64_t prior_candidate_ordinal = 0u;
        for (std::size_t job_index = 0u; job_index < audit_.jobs.size(); ++job_index) {
            const runtime_pipeline_job_use_audit& job = audit_.jobs[job_index];
            const std::uint64_t problem_ordinal = job.anchor_count;
            const std::uint64_t candidate_ordinal = job.range_count;
            std::uint64_t total = 0u;
            for (const std::uint64_t count : job.dispatches) {
                if (count > (std::numeric_limits<std::uint64_t>::max)() - total)
                    return false;
                total += count;
            }
            if (!job.shape_contract_set || job.minimizer_count == 0u || job.target_len == 0u ||
                job.target_position_offset == 0u || problem_ordinal == 0u ||
                candidate_ordinal == 0u ||
                (job_index != 0u && (problem_ordinal < prior_problem_ordinal ||
                                     (problem_ordinal == prior_problem_ordinal &&
                                      candidate_ordinal <= prior_candidate_ordinal))) ||
                job.committed_submissions == 0u || job.committed_submissions > 2u ||
                job.count(shader_kind::align_extend_simple_cigar) != job.committed_submissions ||
                total != job.committed_submissions) {
                return false;
            }
            prior_problem_ordinal = problem_ordinal;
            prior_candidate_ordinal = candidate_ordinal;
        }
        return true;
    }

    /// @brief Checks one physical post-alignment MAPQ dispatch and readback receipt.
    [[nodiscard]] bool is_valid_for_persisted_mapq() const noexcept {
        constexpr std::array required{shader_kind::mapping_quality_reduce};
        if (!is_common_state_valid() ||
            audit_.kind != runtime_pipeline_operation_kind::persisted_mapq ||
            audit_.jobs.size() != 1u || audit_.committed_submissions != 1u ||
            !is_prepared_exactly(required)) {
            return false;
        }
        const runtime_pipeline_job_use_audit& job = audit_.jobs.front();
        std::uint64_t total = 0u;
        for (const std::uint64_t count : job.dispatches) {
            if (count > (std::numeric_limits<std::uint64_t>::max)() - total) {
                return false;
            }
            total += count;
        }
        return job.shape_contract_set && job.minimizer_count != 0u &&
               job.anchor_count <= job.target_len && job.range_count != 0u &&
               job.committed_submissions == 1u &&
               job.count(shader_kind::mapping_quality_reduce) == 1u && total == 1u;
    }

    /// @brief Exercises the collector-to-token causal receipt contract.
    /// @param[in] base_audit Valid immutable Metal archive pipeline audit.
    /// @return Whether the golden and fail-closed receipt scenarios agree.
    // The definition follows runtime_pipeline_use_collector because this test
    // exercises that class's private recording API through their friendship.
    [[nodiscard]] static bool
    self_test_causal_collector_contract(runtime_metal_archive_pipeline_audit base_audit);

  private:
    /// @brief Checks state shared by every operation-specific receipt.
    /// @return Whether the token, environment, prepared receipts, and jobs agree.
    [[nodiscard]] bool is_common_state_valid(bool allow_undispatched_jobs = false) const noexcept {
        if (!issued_ || audit_.operation_identity == 0u ||
            audit_.kind == runtime_pipeline_operation_kind::invalid || audit_.jobs.empty() ||
            audit_.prepared_count == 0u || audit_.prepared_count > audit_.prepared.size() ||
            (!allow_undispatched_jobs && audit_.committed_submissions == 0u) ||
            audit_.committed_command_generations != audit_.committed_submissions ||
            audit_.begun_command_generations != audit_.committed_command_generations ||
            audit_.burned_pending_uses != 0u || audit_.pending || audit_.invalid ||
            audit_.saturated || !audit_.cache_metrics.is_internally_consistent() ||
            audit_.cache_metrics.token_identity != audit_.operation_identity ||
            audit_.api_version == 0u || audit_.driver_version == 0u ||
            (audit_.native_vulkan
                ? !k_native_vulkan_pipeline_receipts ||
                      audit_.seed_sha256 != std::array<std::uint8_t, 32>{} ||
                      audit_.seed_size != 0u || audit_.manifest_checksum != 0u ||
                      audit_.seed_receipt_identity != 0u ||
                      audit_.archive_set != runtime_metal_archive_set_identity{} ||
                      audit_.cache_qualification != runtime_metal_archive_process_qualification{}
                : audit_.seed_size == 0u || audit_.manifest_checksum == 0u ||
                      audit_.seed_receipt_identity == 0u || !audit_.archive_set.is_valid() ||
                      !audit_.cache_qualification.is_internally_consistent() ||
                      audit_.cache_qualification.archive_set != audit_.archive_set) ||
            std::all_of(audit_.device_uuid.begin(),
                        audit_.device_uuid.end(),
                        [](std::uint8_t value) { return value == 0u; }) ||
            std::all_of(audit_.pipeline_cache_uuid.begin(),
                        audit_.pipeline_cache_uuid.end(),
                        [](std::uint8_t value) { return value == 0u; }) ||
            (!audit_.native_vulkan && std::all_of(audit_.seed_sha256.begin(),
                        audit_.seed_sha256.end(),
                        [](std::uint8_t value) { return value == 0u; }))) {
            return false;
        }
        for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
            const runtime_pipeline_prepared_audit& prepared = audit_.prepared[i];
            const runtime_metal_archive_pipeline_receipt* receipt = receipts_[i];
            if (receipt == nullptr || !receipt->is_valid() || prepared.prepared_calls == 0u ||
                prepared.receipt_identity == 0u ||
                !is_shader_variant_key_valid(prepared.shader_key)) {
                return false;
            }
            const runtime_metal_archive_pipeline_audit& pipeline = receipt->audit();
            if (pipeline.native_vulkan != audit_.native_vulkan ||
                pipeline.receipt_identity != prepared.receipt_identity ||
                pipeline.shader_key != prepared.shader_key ||
                pipeline.runtime_vulkan_device_uuid != audit_.device_uuid ||
                pipeline.api_version != audit_.api_version ||
                pipeline.driver_version != audit_.driver_version ||
                pipeline.vendor_id != audit_.vendor_id || pipeline.device_id != audit_.device_id ||
                pipeline.pipeline_cache_uuid != audit_.pipeline_cache_uuid ||
                pipeline.seed_sha256 != audit_.seed_sha256 ||
                pipeline.seed_size != audit_.seed_size ||
                pipeline.manifest_checksum != audit_.manifest_checksum ||
                pipeline.seed_receipt_identity != audit_.seed_receipt_identity ||
                pipeline.expected_archives != audit_.archive_set ||
                pipeline.cache_qualification != audit_.cache_qualification) {
                return false;
            }
        }
        for (const runtime_pipeline_job_use_audit& job : audit_.jobs) {
            if (job.saturated || (!allow_undispatched_jobs && job.committed_submissions == 0u))
                return false;
            if (job.committed_submissions > job.submission_ordinals.size() ||
                (job.committed_submissions == 0u
                     ? job.submission_operation_identity != 0u
                     : job.submission_operation_identity != audit_.operation_identity)) {
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

    /// @brief Reads the preparation-call count for one shader kind.
    /// @param[in] kind Shader kind to find in the prepared receipt set.
    /// @return Recorded call count, or zero when the kind was not prepared.
    [[nodiscard]] std::uint64_t prepared_calls(shader_kind kind) const noexcept {
        const std::uint32_t raw = static_cast<std::uint32_t>(kind);
        for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
            if (audit_.prepared[i].shader_key.kind == raw) {
                return audit_.prepared[i].prepared_calls;
            }
        }
        return 0u;
    }

    /// @brief Checks that the prepared set exactly matches the required shader kinds.
    /// @param[in] required Non-radix shader kinds required once each.
    /// @param[in] radix_prepared_calls Expected call count for each radix shader kind.
    /// @return Whether the prepared set contains exactly the requested entries and counts.
    [[nodiscard]] bool is_prepared_exactly(std::span<const shader_kind> required,
                                           std::uint64_t radix_prepared_calls = 0u) const noexcept {
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

/// @brief Mutable collector used while recording and committing GPU submissions.
class runtime_pipeline_use_collector {
  public:
    /// @brief Prevents copying mutable pipeline-use state.
    runtime_pipeline_use_collector(const runtime_pipeline_use_collector&) = delete;

    /// @brief Prevents copy assignment of mutable pipeline-use state.
    runtime_pipeline_use_collector& operator=(const runtime_pipeline_use_collector&) = delete;

    /// @brief Prevents moving the address-stable collector while commands reference it.
    runtime_pipeline_use_collector(runtime_pipeline_use_collector&&) = delete;

    /// @brief Prevents move assignment of address-stable collector state.
    runtime_pipeline_use_collector& operator=(runtime_pipeline_use_collector&&) = delete;

    /// @brief Burns any uncommitted command generation before releasing storage.
    ~runtime_pipeline_use_collector() {
        burn_pending();
    }

  private:
    /// @brief Dispatch and workgroup deltas recorded before one submission commits.
    struct pending_job {
        std::array<std::uint64_t, k_runtime_pipeline_kind_slots> dispatches{};
        std::array<std::uint64_t, k_runtime_pipeline_kind_slots> workgroups{};
    };

    /// @brief Allocates an admitted collector for one operation and fixed job count.
    /// @param[in] kind Semantic operation family.
    /// @param[in] job_count Number of independently audited jobs.
    /// @param[in] operation_identity Nonzero causal operation identity.
    /// @return Unique address-stable collector owner.
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

    /// @brief Initializes fixed job storage under an acquired capacity permit.
    /// @param[in] kind Semantic operation family.
    /// @param[in] job_count Number of independently audited jobs.
    /// @param[in] operation_identity Nonzero causal operation identity.
    /// @param[in] capacity_permit Admission permit covering collector storage.
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

    /// @brief Measures retained collector and vector capacities.
    /// @return Saturating retained-capacity total in bytes.
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

    /// @brief Shrinks the admission permit to exact retained collector capacity.
    void reconcile_capacity_permit() {
        const std::uint64_t retained = retained_capacity_bytes();
        if (retained == (std::numeric_limits<std::uint64_t>::max)() || !capacity_permit_ ||
            retained > capacity_permit_.bytes()) {
            throw vulkan_runtime_error(
                "runtime pipeline receipt collector exceeded admitted capacity");
        }
        capacity_permit_.shrink_to(retained);
    }

    /// @brief Adds a counter with saturating failure semantics.
    /// @param[in,out] destination Counter to increment or saturate.
    /// @param[in] value Increment value.
    /// @return Whether the addition completed without overflow.
    static bool add_counter(std::uint64_t& destination, std::uint64_t value) noexcept {
        if (value > (std::numeric_limits<std::uint64_t>::max)() - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            return false;
        }
        destination += value;
        return true;
    }

    /// @brief Marks this collector's causal audit invalid.
    void invalidate() noexcept {
        audit_.invalid = true;
    }

    /// @brief Records one prepared immutable pipeline receipt and seals environment identity.
    /// @param[in] key Shader variant prepared for later dispatch.
    /// @param[in] receipt Canonical immutable pipeline receipt.
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
            audit_.native_vulkan = source.native_vulkan;
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
        } else if (source.native_vulkan != audit_.native_vulkan ||
                   source.runtime_vulkan_device_uuid != audit_.device_uuid ||
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

    /// @brief Records immutable input dimensions for one grouped-SoA job.
    /// @param[in] job_index Job slot to update.
    /// @param[in] minimizer_count Query minimizer count.
    /// @param[in] target_len Target length in bases.
    /// @param[in] target_position_offset Global target-coordinate offset.
    void set_shape_input(std::size_t job_index,
                         std::uint64_t minimizer_count,
                         std::uint64_t target_len,
                         std::uint64_t target_position_offset,
                         std::uint32_t binding_window_count = 1u) noexcept {
        if (job_index >= audit_.jobs.size()) {
            invalidate();
            return;
        }
        runtime_pipeline_job_use_audit& job = audit_.jobs[job_index];
        job.minimizer_count = minimizer_count;
        job.target_len = target_len;
        job.target_position_offset = target_position_offset;
        job.binding_window_count = binding_window_count;
        job.shape_contract_set = true;
    }

    /// @brief Records the resulting anchor and chain-range counts for one job.
    /// @param[in] job_index Job slot with an established input shape.
    /// @param[in] anchor_count Emitted anchor count.
    /// @param[in] range_count Constructed chain-range count.
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

    /// @brief Seals the authorization identity plus completed host order proof.
    void set_seed_anchor_authentication(
        std::size_t job_index,
        std::uint64_t query_authority_identity,
        const seed_anchor_canonical_order_audit& order_audit) noexcept {
        if (job_index >= audit_.jobs.size() ||
            !audit_.jobs[job_index].shape_contract_set || query_authority_identity == 0u ||
            order_audit.anchor_count != audit_.jobs[job_index].anchor_count) {
            invalidate();
            return;
        }
        runtime_pipeline_job_use_audit& job = audit_.jobs[job_index];
        job.query_authority_identity = query_authority_identity;
        job.anchor_digest_lo = order_audit.digest_lo;
        job.anchor_digest_hi = order_audit.digest_hi;
        job.anchor_order_checked = order_audit.canonical_order_checked;
        job.anchor_order_accepted = order_audit.canonical_order_accepted;
    }

    /// @brief Begins one causal command-buffer generation.
    /// @param[in] command_buffer Command buffer whose dispatches will be recorded.
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

    /// @brief Records one dispatch against a prepared receipt in the active generation.
    /// @param[in] command_buffer Active command buffer.
    /// @param[in] job_index Job slot receiving the dispatch.
    /// @param[in] key Prepared shader variant key.
    /// @param[in] receipt Canonical receipt corresponding to `key`.
    /// @param[in] group_count Dispatched workgroup count.
    /// @param[in] observed_generation Optional caller-observed command generation.
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
    /// @brief Commits every pending dispatch after successful submission completion.
    /// @param[in] command_buffer Active command buffer whose generation completed.
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

    /// @brief Discards pending dispatches and accounts them as burned uses.
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

    /// @brief Burns an active generation before command-buffer reset and invalidates it.
    /// @param[in] command_buffer Command buffer about to be reset.
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
    /// @brief Transfers the completed audit, receipt links, and retained permit into a token.
    /// @return Move-only causal operation token.
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
    const std::array<shader_variant_key, 13> keys{
        make_shader_key(shader_kind::ordered_sketch_tile, 384u, 64u, 10u),
        make_shader_key(shader_kind::prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(shader_kind::prefix_scan_add, 384u, 128u, 1u),
        make_shader_key(
            shader_kind::grouped_soa_seed_count, 384u, 128u, k_grouped_soa_seed_cache_abi_revision),
        make_shader_key(
            shader_kind::grouped_soa_seed_emit, 384u, 128u, k_grouped_soa_seed_cache_abi_revision),
        make_shader_key(shader_kind::chain_range_build, 384u, 1u, 1u),
        make_shader_key(shader_kind::coordinate_reduce, 384u, 128u, 1u),
        make_shader_key(shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u),
        make_shader_key(shader_kind::radix_sort_histogram, 384u, 1u, 1u),
        make_shader_key(shader_kind::radix_sort_scan, 384u, 1u, 1u),
        make_shader_key(shader_kind::radix_sort_scatter, 384u, 1u, 1u),
        make_shader_key(
            shader_kind::align_extend_simple_cigar, 384u, 64u, k_gpu_alignment_extension_max_len),
        make_shader_key(
            shader_kind::mapping_quality_reduce, 384u, 1u, k_gpu_persisted_mapq_candidate_capacity),
    };
    if (!std::all_of(keys.begin(), keys.end(), [](const shader_variant_key& key) {
            return is_shader_variant_key_valid(key);
        })) {
        return false;
    }
    std::array<runtime_metal_archive_pipeline_receipt, 13> receipts{};
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
    // Creates stable non-null command identities without allocating Vulkan objects.
    const auto command = [](std::uintptr_t identity) {
        return reinterpret_cast<VkCommandBuffer>(identity);
    };
    // Registers the three pipelines required by query sketching.
    const auto prepare_query = [&keys, &receipts](runtime_pipeline_use_collector& collector) {
        for (std::size_t i = 0u; i < 3u; ++i) {
            collector.prepare(keys[i], &receipts[i]);
        }
    };
    // Builds a fully committed two-submission query token.
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

    // Builds one malformed collector state selected by `mode` for rejection checks.
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
        collector->set_shape_input(2u, 1u, 65536u, 0u, 3u);
        collector->set_shape_outcome(0u, 0u, 0u);
        collector->set_shape_outcome(1u, 1u, 0u);
        collector->set_shape_outcome(2u, 1u, 1u);

        collector->begin(command(11u));
        for (std::size_t job = 0u; job < 3u; ++job) {
            for (std::size_t window = 0u; window < (job == 2u ? 3u : 1u); ++window)
                collector->use(command(11u), job, keys[3], &receipts[3], 1u);
            collector->use(command(11u), job, keys[1], &receipts[1], 1u);
            collector->use(command(11u), job, keys[2], &receipts[2], 1u);
            collector->use(command(11u), job, keys[2], &receipts[2], 1u);
        }
        collector->commit(command(11u));

        collector->begin(command(12u));
        for (std::size_t job = 1u; job < 3u; ++job) {
            for (std::size_t window = 0u; window < (job == 2u ? 3u : 1u); ++window)
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
    for (const auto kind : {shader_kind::grouped_soa_seed_count, shader_kind::grouped_soa_seed_emit}) {
        auto& count = shape.audit_.jobs[2].dispatches[static_cast<std::size_t>(kind)];
        --count;
        if (shape.is_valid_for_grouped_soa_coordinate()) return false;
        ++count;
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

    // MPS stops after the genuine seed emit/radix prefix. Empty queries retain
    // authenticated zero-length jobs but own no fabricated dispatch receipt.
    runtime_pipeline_operation_token seed_only{};
    runtime_pipeline_operation_token rejected_seed_only{};
    runtime_pipeline_operation_token empty_seed_only{};
    try {
        const auto make_seed_only = [&keys, &receipts, command](std::uint64_t identity,
                                                                bool accepted_order) {
            auto collector = runtime_pipeline_use_collector::create(
                runtime_pipeline_operation_kind::grouped_soa_seed_anchor_roundtrip,
                3u,
                identity);
            constexpr std::array<std::size_t, 4u> base_slots{3u, 4u, 1u, 2u};
            for (const std::size_t slot : base_slots)
                collector->prepare(keys[slot], &receipts[slot]);
            for (std::size_t slot = 8u; slot < 11u; ++slot)
                collector->prepare(keys[slot], &receipts[slot]);

            for (std::size_t job = 0u; job < 3u; ++job)
                collector->set_shape_input(job, job == 0u ? 0u : 1u, 255u, 0u, 2u);
            collector->set_shape_outcome(0u, 0u, 0u);
            collector->set_shape_outcome(1u, 0u, 0u);
            collector->set_shape_outcome(2u, 1u, 0u);
            const seed_anchor_canonical_order_audit empty_audit{
                .anchor_count = 0u,
                .canonical_order_checked = true,
                .canonical_order_accepted = true,
            };
            const seed_anchor_canonical_order_audit anchor_audit{
                .anchor_count = 1u,
                .digest_lo = 0x1234u,
                .digest_hi = 0x5678u,
                .canonical_order_checked = true,
                .canonical_order_accepted = accepted_order,
            };
            collector->set_seed_anchor_authentication(0u, 700u, empty_audit);
            collector->set_seed_anchor_authentication(1u, 700u, empty_audit);
            collector->set_seed_anchor_authentication(2u, 700u, anchor_audit);

            const VkCommandBuffer count_command =
                command(static_cast<std::uintptr_t>(identity));
            collector->begin(count_command);
            for (std::size_t job = 1u; job < 3u; ++job) {
                for (std::size_t window = 0u; window < 2u; ++window)
                    collector->use(count_command, job, keys[3], &receipts[3], 1u);
                collector->use(count_command, job, keys[1], &receipts[1], 1u);
                collector->use(count_command, job, keys[2], &receipts[2], 1u);
                collector->use(count_command, job, keys[2], &receipts[2], 1u);
            }
            collector->commit(count_command);

            const VkCommandBuffer emit_command =
                command(static_cast<std::uintptr_t>(identity + 1u));
            collector->begin(emit_command);
            for (std::size_t window = 0u; window < 2u; ++window)
                collector->use(emit_command, 2u, keys[4], &receipts[4], 1u);
            for (std::size_t pass = 0u; pass < 2u; ++pass) {
                for (std::size_t slot = 8u; slot < 11u; ++slot)
                    collector->use(emit_command, 2u, keys[slot], &receipts[slot], 1u);
            }
            collector->commit(emit_command);
            return collector->seal();
        };
        seed_only = make_seed_only(207u, true);
        rejected_seed_only = make_seed_only(209u, false);
        auto empty_collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::grouped_soa_seed_anchor_roundtrip, 2u, 211u);
        constexpr std::array<std::size_t, 4u> empty_base_slots{3u, 4u, 1u, 2u};
        for (const std::size_t slot : empty_base_slots)
            empty_collector->prepare(keys[slot], &receipts[slot]);
        const seed_anchor_canonical_order_audit empty_audit{
            .anchor_count = 0u,
            .canonical_order_checked = true,
            .canonical_order_accepted = true,
        };
        for (std::size_t job = 0u; job < 2u; ++job) {
            empty_collector->set_shape_input(job, 0u, 255u, 0u);
            empty_collector->set_shape_outcome(job, 0u, 0u);
            empty_collector->set_seed_anchor_authentication(job, 701u, empty_audit);
        }
        empty_seed_only = empty_collector->seal();
    } catch (...) {
        return false;
    }
    if (!seed_only.is_valid_for_grouped_soa_seed_anchor_roundtrip() ||
        seed_only.audit_.committed_submissions != 2u ||
        seed_only.audit_.jobs[0].committed_submissions != 0u ||
        seed_only.audit_.jobs[1].committed_submissions != 1u ||
        seed_only.audit_.jobs[2].committed_submissions != 2u ||
        !empty_seed_only.is_valid_for_grouped_soa_seed_anchor_roundtrip() ||
        empty_seed_only.audit_.committed_submissions != 0u ||
        rejected_seed_only.is_valid_for_grouped_soa_seed_anchor_roundtrip()) {
        return false;
    }

    for (const auto kind : {shader_kind::grouped_soa_seed_count, shader_kind::grouped_soa_seed_emit}) {
        auto& count = seed_only.audit_.jobs[2].dispatches[static_cast<std::size_t>(kind)];
        --count;
        if (seed_only.is_valid_for_grouped_soa_seed_anchor_roundtrip()) return false;
        ++count;
    }

    // One logical alignment operation may use a trace-free screening wave and
    // a second traceback wave. The token must count physical submissions once
    // globally and once for every job actually touched by that submission.
    runtime_pipeline_operation_token alignment{};
    try {
        auto collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::persisted_alignment_cigar, 3u, 212u);
        for (std::size_t job = 0u; job < 3u; ++job) {
            collector->set_shape_input(job, 20'000u, 20'000u, 9'000u + job);
            collector->set_shape_outcome(job, 1u + 2u * job, 1u);
        }
        collector->prepare(keys[11], &receipts[11]);
        collector->begin(command(21u));
        for (std::size_t job = 0u; job < 3u; ++job) {
            collector->use(command(21u), job, keys[11], &receipts[11], 1u);
        }
        collector->commit(command(21u));
        collector->prepare(keys[11], &receipts[11]);
        collector->begin(command(22u));
        for (std::size_t job = 1u; job < 3u; ++job) {
            collector->use(command(22u), job, keys[11], &receipts[11], 1u);
        }
        collector->commit(command(22u));
        alignment = collector->seal();
    } catch (...) {
        return false;
    }
    constexpr std::array<std::uint64_t, 3> expected_alignment_submissions{1u, 2u, 2u};
    if (!alignment.is_valid_for_persisted_alignment_cigar() ||
        alignment.audit_.committed_submissions != 2u ||
        alignment.prepared_calls(shader_kind::align_extend_simple_cigar) != 2u) {
        return false;
    }
    for (std::size_t job = 0u; job < alignment.audit_.jobs.size(); ++job) {
        if (alignment.audit_.jobs[job].committed_submissions !=
                expected_alignment_submissions[job] ||
            alignment.audit_.jobs[job].count(shader_kind::align_extend_simple_cigar) !=
                expected_alignment_submissions[job]) {
            return false;
        }
    }

    // Post-alignment MAPQ is a distinct one-submission causal operation.  Its
    // generic shape fields bind problem/candidate/alignment counts on input
    // and published-count/result-digest evidence on output.
    runtime_pipeline_operation_token mapq{};
    runtime_pipeline_operation_token rejected_mapq{};
    try {
        const auto make_mapq = [&keys, &receipts, command](std::uint64_t identity,
                                                           std::uint64_t result_digest) {
            auto collector = runtime_pipeline_use_collector::create(
                runtime_pipeline_operation_kind::persisted_mapq, 1u, identity);
            collector->prepare(keys[12], &receipts[12]);
            collector->set_shape_input(0u, 3u, 7u, 5u);
            collector->begin(command(static_cast<std::uintptr_t>(identity)));
            collector->use(
                command(static_cast<std::uintptr_t>(identity)), 0u, keys[12], &receipts[12], 3u);
            collector->commit(command(static_cast<std::uintptr_t>(identity)));
            collector->set_shape_outcome(0u, 4u, result_digest);
            return collector->seal();
        };
        mapq = make_mapq(222u, 0x1234u);
        rejected_mapq = make_mapq(223u, 0u);
    } catch (...) {
        return false;
    }
    if (!mapq.is_valid_for_persisted_mapq() || mapq.audit_.committed_submissions != 1u ||
        mapq.prepared_calls(shader_kind::mapping_quality_reduce) != 1u ||
        mapq.audit_.jobs.front().minimizer_count != 3u ||
        mapq.audit_.jobs.front().target_len != 7u ||
        mapq.audit_.jobs.front().target_position_offset != 5u ||
        mapq.audit_.jobs.front().anchor_count != 4u ||
        mapq.audit_.jobs.front().range_count != 0x1234u ||
        rejected_mapq.is_valid_for_persisted_mapq()) {
        return false;
    }

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    // Reuse this collector contract's receipt set to distinguish score-only
    // chaining from missing, uncommitted, or coordinate-reducer work.
    try {
        const auto make_chaining = [&keys, &receipts, command](int mode) {
            const auto identity = static_cast<std::uint64_t>(230 + mode);
            auto collector = runtime_pipeline_use_collector::create(
                runtime_pipeline_operation_kind::chaining_tiled_dp, 1u, identity);
            collector->prepare(keys[7], &receipts[7]);
            collector->set_shape_input(0u, 0u, 0u, 0u);
            collector->set_shape_outcome(0u, 64u, mode == 5 ? 65u : 2u);
            const auto cmd = command(static_cast<std::uintptr_t>(identity));
            collector->begin(cmd);
            if (mode != 1) {
                collector->use(cmd, 0u, keys[7],
                               mode == 4 ? &receipts[6] : &receipts[7],
                               mode == 6 ? 1u : 2u);
            }
            if (mode == 3) {
                collector->prepare(keys[6], &receipts[6]);
                collector->use(cmd, 0u, keys[6], &receipts[6], 1u);
            }
            if (mode != 2)
                collector->commit(cmd);
            return collector->seal();
        };
        auto chain_token = make_chaining(0);
        if (!chain_token.is_valid_for_chaining_tiled_dp() ||
            !chain_token.is_valid_for_chaining_tiled_dp(2u) ||
            !chain_token.is_valid_for_chaining_tiled_dp(65'535u) ||
            chain_token.is_valid_for_chaining_tiled_dp(1u) ||
            chain_token.is_valid_for_chaining_tiled_dp(0u) ||
            chain_token.audit().jobs.front().anchor_count != 64u ||
            chain_token.audit().jobs.front().range_count != 2u) {
            return false;
        }
        auto moved_chain = std::move(chain_token);
        if (chain_token.is_valid_for_chaining_tiled_dp() ||
            !moved_chain.is_valid_for_chaining_tiled_dp()) {
            return false;
        }
        for (int mode = 1; mode <= 5; ++mode) {
            if (make_chaining(mode).is_valid_for_chaining_tiled_dp())
                return false;
        }
        auto limited_chain = make_chaining(6);
        if (!limited_chain.is_valid_for_chaining_tiled_dp(1u) ||
            limited_chain.is_valid_for_chaining_tiled_dp(2u)) {
            return false;
        }
    } catch (...) {
        return false;
    }

    // Synthetic collector contracts, not measured execution evidence. Exercise
    // both existing MPF frontends and the count-only zero-hit outcome.
    try {
        const std::array extra_keys{
            make_shader_key(shader_kind::seed_lookup_anchors, 384u, 128u, 256u),
            make_shader_key(shader_kind::sort_anchors_bitonic, 384u, 128u, 256u),
            make_shader_key(shader_kind::seed_count, 384u, 128u, 1u),
            make_shader_key(shader_kind::seed_emit, 384u, 128u, 1u),
        };
        std::array<runtime_metal_archive_pipeline_receipt, 4u> extra_receipts{};
        for (std::size_t index = 0u; index < extra_receipts.size(); ++index) {
            auto audit = base_audit;
            audit.receipt_identity = 4000u + index;
            audit.scope_identity = 5000u + index;
            audit.shader_key = extra_keys[index];
            extra_receipts[index] = runtime_metal_archive_pipeline_receipt{std::move(audit)};
            if (!extra_receipts[index].is_valid()) {
                return false;
            }
        }
        const auto make_mpf = [&extra_keys, &extra_receipts, &keys, &receipts, command](
            bool fused, bool bitonic, bool empty, int mutation) {
            const std::uint64_t identity = 500u + static_cast<std::uint64_t>(mutation);
            auto collector = runtime_pipeline_use_collector::create(
                runtime_pipeline_operation_kind::mpf_seed_anchor_roundtrip, 1u, identity);
            collector->set_shape_input(0u, 17u, 255u, 0u);
            collector->set_shape_outcome(0u, empty ? 0u : 1u, 0u);
            seed_anchor_canonical_order_audit order{
                .anchor_count = empty ? 0u : 1u,
                .canonical_order_checked = true,
                .canonical_order_accepted = mutation != 2,
            };
            collector->set_seed_anchor_authentication(0u, mutation == 3 ? 0u : 701u, order);
            if (fused) {
                collector->prepare(extra_keys[2], &extra_receipts[2]);
                collector->prepare(extra_keys[3], &extra_receipts[3]);
                collector->prepare(keys[1], &receipts[1]);
                collector->prepare(keys[2], &receipts[2]);
            } else {
                collector->prepare(extra_keys[0], &extra_receipts[0]);
            }
            const auto first = command(static_cast<std::uintptr_t>(identity));
            collector->begin(first);
            if (mutation != 1) {
                collector->use(first, 0u, extra_keys[fused ? 2u : 0u],
                               &extra_receipts[fused ? 2u : 0u], 1u);
            }
            if (fused) {
                collector->use(first, 0u, keys[1], &receipts[1], 1u);
                collector->use(first, 0u, keys[2], &receipts[2], 1u);
                collector->use(first, 0u, keys[2], &receipts[2], 1u);
                collector->commit(first);
            }
            if (!empty) {
                const auto emit = fused ? command(static_cast<std::uintptr_t>(identity + 10'000u))
                                        : first;
                if (fused) {
                    collector->begin(emit);
                    collector->use(emit, 0u, extra_keys[3], &extra_receipts[3], 1u);
                }
                if (bitonic) {
                    collector->prepare(extra_keys[1], &extra_receipts[1]);
                    collector->use(emit, 0u, extra_keys[1], &extra_receipts[1], 1u);
                } else {
                    for (std::size_t slot = 8u; slot <= 10u; ++slot) {
                        collector->prepare(keys[slot], &receipts[slot]);
                    }
                    const auto passes = runtime_grouped_soa_radix_pass_count(0u, 255u);
                    for (std::uint32_t pass = 0u; pass < *passes; ++pass) {
                        for (std::size_t slot = 8u; slot <= 10u; ++slot) {
                            collector->use(emit, 0u, keys[slot], &receipts[slot], 1u);
                        }
                    }
                }
                collector->commit(emit);
            } else if (!fused) {
                collector->commit(first);
            }
            return collector->seal();
        };
        for (const bool fused : {false, true}) {
            auto token = make_mpf(fused, false, false, 0);
            if (!token.is_valid_for_mpf_seed_anchor_roundtrip(1u) ||
                token.is_valid_for_mpf_seed_anchor_roundtrip(0u)) {
                return false;
            }
            auto moved = std::move(token);
            if (token.is_valid_for_mpf_seed_anchor_roundtrip(1u) ||
                !moved.is_valid_for_mpf_seed_anchor_roundtrip(1u)) {
                return false;
            }
            for (int mutation = 1; mutation <= 3; ++mutation) {
                if (make_mpf(fused, false, false, mutation)
                        .is_valid_for_mpf_seed_anchor_roundtrip(1u)) {
                    return false;
                }
            }
        }
        if (!make_mpf(false, true, false, 0).is_valid_for_mpf_seed_anchor_roundtrip(1u) ||
            !make_mpf(true, false, true, 0).is_valid_for_mpf_seed_anchor_roundtrip(1u) ||
            make_mpf(false, false, true, 0).is_valid_for_mpf_seed_anchor_roundtrip(1u) ||
            make_mpf(true, true, false, 0).is_valid_for_mpf_seed_anchor_roundtrip(1u)) {
            return false;
        }
    } catch (...) {
        return false;
    }
#endif

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
    std::array<const runtime_metal_archive_pipeline_receipt*, 13> receipt_pointers{};
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

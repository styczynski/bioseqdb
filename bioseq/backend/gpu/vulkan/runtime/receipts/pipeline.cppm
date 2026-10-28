/**
 * @file pipeline.cppm
 * @brief Records which GPU pipelines an operation used and which submissions completed.
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

/// Number of shader kinds that each job can count
inline constexpr std::size_t k_runtime_pipeline_kind_slots = 64u;
/// Maximum number of distinct pipelines recorded for one operation.
inline constexpr std::size_t k_runtime_pipeline_prepared_slots = 16u;

/// Ordered submission phases of a native seed-and-coordinate job. A phase runs
/// at most once per job; shared wave submissions retain their operation ordinal.
enum class seed_coordinate_submission_phase : std::size_t {
    count_candidates,
    emit_candidates,
    sort_candidates,
    native_coverage,
    verify_count,
    verify_emit,
    build_ranges,
    chain_coordinates,
    count
};
inline constexpr std::size_t k_seed_coordinate_submission_phase_count =
    static_cast<std::size_t>(seed_coordinate_submission_phase::count);
inline constexpr std::array<std::string_view, k_seed_coordinate_submission_phase_count>
    seed_coordinate_submission_phase_names{
        "count", "emit", "sort", "native_coverage", "verify_count", "verify_emit",
        "range", "chain_coordinate"};
inline constexpr std::array grouped_soa_coordinate_required_shaders{
    shader_kind::grouped_soa_seed_count, shader_kind::grouped_soa_seed_emit,
    shader_kind::prefix_scan_block, shader_kind::prefix_scan_add,
    shader_kind::chain_range_build, shader_kind::coordinate_reduce,
    shader_kind::chain_tiled_dp_u64};
inline constexpr std::array seed_radix_required_shaders{
    shader_kind::radix_sort_histogram, shader_kind::radix_sort_scan,
    shader_kind::radix_sort_scatter};
inline constexpr std::array seed_verification_required_shaders{
    shader_kind::seed_verify_count, shader_kind::seed_verify_emit};
inline constexpr std::size_t k_verified_coordinate_pipeline_count =
    grouped_soa_coordinate_required_shaders.size() + seed_radix_required_shaders.size() +
    seed_verification_required_shaders.size();

/// @brief Counts radix-sort passes for target positions and the strand bit.
/// Each pass sorts one byte of an anchor reference. The target span determines
/// how many bytes can contain a position or strand bit.
/// @return No value if the span is empty, overflows, or reaches the reserved strand bit.
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

/// @brief GPU operations that can issue a completion  receipt.
enum class runtime_pipeline_operation_kind : std::uint8_t {
    invalid,                   ///< Unissued or moved-from operation.
    query_sketch,              ///< Builds query minimizers with count, prefix-scan, and emit passes.
    grouped_soa_coordinate,    ///< Finds mapping coordinates from the stored grouped minimizer index.
    persisted_alignment_cigar, ///< Aligns packed sequence windows and produces CIGAR strings.
    persisted_mapq,            ///< Classifies alignments and computes mapping quality (MAPQ).
    source_direct_coordinate,  ///< Builds seeds, chains, and coordinates directly from source sequences.
    grouped_soa_seed_anchor_roundtrip, ///< Counts and sorts anchors from the stored grouped, minimizer index.
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    chaining_tiled_dp,          ///< Computes chaining scores and predecessor links with tiled dynamic programming.
    mpf_seed_anchor_roundtrip,  ///< Emits and sorts anchors through the MPF seed frontend.
#endif
};

/// @brief Cache loading and pipeline creation costs assigned to one operation.
struct runtime_gpu_cache_metrics_audit {
    std::uint64_t token_identity = 0u;
    std::uint64_t gpu_pipeline_cache_loads = 0u;
    std::uint64_t gpu_pipeline_cache_seed_bytes = 0u;
    std::uint64_t gpu_pipeline_cache_load_ns = 0u;
    std::uint64_t gpu_pipeline_creates = 0u;
    std::uint64_t gpu_pipeline_creation_ns = 0u;
    /// A counter overflow makes these metrics unusable.
    bool saturated = false;

    /// @brief Checks the operation ID and  tpat recorded loads and creates have timings.
    /// A seed load must also have a byte count. At most one seed load is allowed.
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

    /// @brief Checks that each metric fits its limit for the same operation.
    /// Both records must pass their consistency checks.
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

/// @brief Prepares cache costs for reporting after the final runtime checks.
/// Preparing a plan leaves the shared reporting flags unchanged If an operation
/// fails, a later successful operation can still report these costs.
/// The caller holds the runtime mutex from preparation through commit.
class runtime_gpu_cache_metrics_claim_plan {
  public:
    /// @brief Creates a plan that cannot be committed.
    runtime_gpu_cache_metrics_claim_plan() = default;

    /// @brief Keeps a metric claim from being copied.
    runtime_gpu_cache_metrics_claim_plan(const runtime_gpu_cache_metrics_claim_plan&) = delete;

    /// @brief Keeps assignment  from duplicating a metric claim.
    runtime_gpu_cache_metrics_claim_plan&
    operator=(const runtime_gpu_cache_metrics_claim_plan&) = delete;

    /// @brief Takes the pending claims from `other` and leaves it invalid.
    runtime_gpu_cache_metrics_claim_plan(runtime_gpu_cache_metrics_claim_plan&& other) noexcept
        : audit_(std::exchange(other.audit_, {})), claims_(std::exchange(other.claims_, {})),
          claim_count_(std::exchange(other.claim_count_, 0u)),
          claim_seed_(std::exchange(other.claim_seed_, false)),
          valid_(std::exchange(other.valid_, false)),
          committed_(std::exchange(other.committed_, false)) {}

    /// @brief Replaces this plan with the pending claims from `other`.
    /// The source becomes invalid.
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

    /// @brief Selects seed and pipeline costs that have not yet been reported.
    /// The receipts must be distinct and refer to the same seed. Their totals must
    /// fit `maximum`, which belongs to `token_identity`.
    /// @param[in] seed_already_claimed Whether a previous operation reported the seed load.
    /// @return An invalid plan if the receipts or totals fail these checks.
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

    /// @brief Checks whether the prepared metrics can be committed.
    [[nodiscard]] bool is_valid() const noexcept {
        return valid_ && audit_.is_internally_consistent();
    }

    /// @brief Reports whether this plan has already  marked its costs as reported.
    [[nodiscard]] bool is_committed() const noexcept {
        return committed_;
    }

    /// @brief Returns the costs thms plan would report.
    /// The reference belongs to this plan.
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& audit() const noexcept {
        return audit_;
    }

    /// @brief Marks the selected seed and pipeline costs as reported once.
    /// The caller must hold the runtime mutex. Repeated calls do nothing.
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

/// @brief Cache costs that, one accepted operation may report once.
class runtime_gpu_cache_metrics_delta {
  public:
    /// @brief Creates an empty metrics record that cannot be consumed.
    runtime_gpu_cache_metrics_delta() = default;

    /// @brief Prevents two copies from reporting the same costs.
    runtime_gpu_cache_metrics_delta(const runtime_gpu_cache_metrics_delta&) = delete;

    /// @brief Prevents assignment from duplicating reportable costs.
    runtime_gpu_cache_metrics_delta& operator=(const runtime_gpu_cache_metrics_delta&) = delete;

    /// @brief Takes the metrics from `other` and leaves it invalid
    runtime_gpu_cache_metrics_delta(runtime_gpu_cache_metrics_delta&& other) noexcept
        : audit_(std::exchange(other.audit_, {})), issued_(std::exchange(other.issued_, false)) {}

    /// @brief Replaces these metrics with those from `other`.
    /// The source becomes invalid.
    runtime_gpu_cache_metrics_delta& operator=(runtime_gpu_cache_metrics_delta&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            issued_ = std::exchange(other.issued_, false);
        }
        return *this;
    }

    /// @brief Checks that these metrics were issued and have not been consumed.
    /// Their counters must  also be consistent.
    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && audit_.is_internally_consistent();
    }

    /// @brief Returns the recorded costs until this object is consumed or moved.
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& audit() const noexcept {
        return audit_;
    }

    /// @brief Clears the metrics after reporting them.
    /// The caller must validate them first.
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

/// @brief Pipeline cache policy recorded for an accepted GPU operation.
enum class runtime_pipeline_cache_policy : std::uint8_t {
    invalid,                   ///< No accepted cache policy.
    immutable_preseed_all_hit, ///< All pipelines loaded from the fixed seed cache.
    native_vulkan,             ///< Native Vulkan execution without Metal archive checks.
};

/// @brief GPU, driver, and cache identity recorded when an operation is accepted.
struct runtime_gpu_execution_environment_snapshot {
    bool gpu_environment_present = false;
    std::array<std::uint8_t, VK_UUID_SIZE> gpu_device_uuid{};
    std::array<char, 128> gpu_driver_identity{};
    runtime_pipeline_cache_policy cache_policy = runtime_pipeline_cache_policy::invalid;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    bool pipeline_cache_all_hit = false;

    /// @brief Checks that the device and driver are identified and cache fields match the policy.
    /// Metal requires an identified seed  and cach hits for every pipeline.
    /// Native Vulkan requires the Metal seed fields to be empty.
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

/// @brief Identifies a prepared pipeline and counts its uses of `prepare`.
struct runtime_pipeline_prepared_audit {
    shader_variant_key shader_key{};
    std::uint64_t receipt_identity = 0u;
    /// Successful `prepare` call count for this shader key within the operation.
    std::uint64_t prepared_calls = 0u;
};

/// @brief Dispatch counts and input/output sizes recorded for one GPU job.
struct runtime_pipeline_job_use_audit {
    std::array<std::uint64_t, k_runtime_pipeline_kind_slots> dispatches{};
    std::array<std::uint64_t, k_runtime_pipeline_kind_slots> workgroups{};
    std::uint64_t submission_operation_identity = 0u;
    std::array<std::uint64_t, k_seed_coordinate_submission_phase_count> submission_ordinals{};
    std::uint64_t committed_submissions = 0u;
    std::uint64_t minimizer_count = 0u;
    std::uint64_t target_len = 0u;
    std::uint64_t target_position_offset = 0u;
    std::uint64_t anchor_count = 0u;
    std::uint64_t seed_candidate_count = 0u;
    std::uint64_t seed_verified_count = 0u;
    std::uint64_t seed_emitted_count = 0u;
    std::uint64_t seed_coverage_interval_count = 0u;
    bool seed_native_verification_set = false;
    std::uint64_t range_count = 0u;
    std::uint32_t binding_window_count = 0u; ///< Number of grouped-index windows needing separate count and emit dispatches.
    std::uint64_t query_authority_identity = 0u;
    std::uint64_t anchor_digest_lo = k_seed_anchor_digest_seed_low;
    std::uint64_t anchor_digest_hi = k_seed_anchor_digest_seed_high;
    bool anchor_order_checked = false;
    bool anchor_order_accepted = false;
    /// `set_shape_input` has recorded this job's input sizes.
    bool shape_contract_set = false;
    /// Set when a per-job counter increment overflowed.
    bool saturated = false;

    /// @brief Returns completed dispatches for `kind`, or zero if the kind is out of range.
    [[nodiscard]] std::uint64_t count(shader_kind kind) const noexcept {
        const std::size_t slot = static_cast<std::size_t>(kind);
        return slot < dispatches.size() ? dispatches[slot] : 0u;
    }

    /// Checks that native verification conserved survivors and ran its complete
    /// count/scan/emit protocol, including when every candidate was rejected.
    [[nodiscard]] bool has_valid_seed_native_verification() const noexcept {
        const bool has_candidates = seed_candidate_count != 0u;
        const bool has_coverage = seed_coverage_interval_count != 0u;
        return seed_native_verification_set == has_candidates &&
               seed_candidate_count <= (std::numeric_limits<std::uint32_t>::max)() &&
               seed_verified_count <= seed_candidate_count &&
               seed_emitted_count == seed_verified_count && anchor_count == seed_verified_count &&
               (!has_coverage || seed_coverage_interval_count == seed_candidate_count) &&
               count(shader_kind::seed_verify_count) == (has_candidates ? 1u : 0u) &&
               count(shader_kind::seed_verify_emit) == (has_candidates ? 1u : 0u) &&
               count(shader_kind::seed_native_coverage) == (has_coverage ? 1u : 0u);
    }
};

/// @brief Pipelines, completed submissions, and GPU identity recorded for an operation.
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
    /// Dispatches discarded before their submission was, accepted.
    std::uint64_t burned_pending_uses = 0u;
    /// The current command recording contains dispatches awaiting completion.
    bool pending = false;
    /// A recording or completion check failed.
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

/// @brief Records prepared pipelines and completed GPU dispatches for an  operation
class runtime_pipeline_use_collector;
/// @brief Receipt for coordinates produced from the stored grouped minimizer index.
class persisted_grouped_soa_coordinate_operation_receipt;

/// @brief Records an operation's completed dispatches and the pipelines they used.
/// The runtime accepts each submission after waiting for its fence and checking
/// the GPU state. Moving the token transfers its record and memory reservation.
class runtime_pipeline_operation_token {
  public:
    /// @brief Creates an invalid operation token.
    runtime_pipeline_operation_token() = default;

    /// @brief Keeps the operation receipt from being copied.
    runtime_pipeline_operation_token(const runtime_pipeline_operation_token&) = delete;

    /// @brief Keeps assignment from duplicating an operation receipr.
    runtime_pipeline_operation_token& operator=(const runtime_pipeline_operation_token&) = delete;

    /// @brief Takes the operation record and memory reservation from `other`.
    /// The source becomes  invalid.
    runtime_pipeline_operation_token(runtime_pipeline_operation_token&& other) noexcept
        : capacity_permit_(std::move(other.capacity_permit_)), audit_(std::move(other.audit_)),
          receipts_(std::exchange(other.receipts_, {})),
          issued_(std::exchange(other.issued_, false)) {
        other.audit_ = {};
    }

    /// @brief Replaces this receipt with the record and reservation from `other`.
    /// The source becomes invalid.
    runtime_pipeline_operation_token& operator=(runtime_pipeline_operation_token&& other) noexcept {
        if (this != &other) {
            // Release the old vector while its memory reservation still exists.
            // Moving the permit first would release that reservation too soon.
            audit_ = std::move(other.audit_);
            capacity_permit_ = std::move(other.capacity_permit_);
            receipts_ = std::exchange(other.receipts_, {});
            issued_ = std::exchange(other.issued_, false);
            other.audit_ = {};
        }
        return *this;
    }

    /// @brief Returns the operation record owned by this token.
    [[nodiscard]] const runtime_pipeline_operation_audit& audit() const noexcept {
        return audit_;
    }
    /// @brief Checks the two completed submissions that count and emit query minimizers.
    /// Requires the sketch and prefix-scan pipelines with exactly their expected dispatches.
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
    /// Checks for one completed dispatch that computes chaining scores and predecessors.
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

    /// Also checks that chaining uses the workgroup count allowed by  the device.
    [[nodiscard]] bool is_valid_for_chaining_tiled_dp(
        std::uint32_t max_workgroup_count_x) const noexcept {
        if (max_workgroup_count_x == 0u || !is_valid_for_chaining_tiled_dp()) {
            return false;
        }
        const auto& job = audit_.jobs.front();
        return job.workgroups[static_cast<std::size_t>(shader_kind::chain_tiled_dp_u64)] ==
               std::min(job.range_count, static_cast<std::uint64_t>(max_workgroup_count_x));
    }

    /// Checks raw candidate generation, native verification and survivor ordering.
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
            job.range_count != 0u || job.query_authority_identity == 0u ||
            !job.anchor_order_checked || !job.anchor_order_accepted ||
            !job.has_valid_seed_native_verification()) {
            return false;
        }
        const auto count = [&job](shader_kind kind) noexcept { return job.count(kind); };
        const bool prefix_driven = count(shader_kind::seed_lookup_anchors) != 0u;
        const bool bitonic = count(shader_kind::sort_anchors_bitonic) != 0u;
        const std::uint64_t verification_calls = job.seed_candidate_count != 0u ? 1u : 0u;
        const std::uint64_t coverage_calls = job.seed_coverage_interval_count != 0u ? 1u : 0u;
        const std::uint64_t seed_groups = (job.minimizer_count + 127u) / 128u;
        std::uint64_t expected_dispatches = 5u * verification_calls + coverage_calls;
        std::uint64_t expected_submissions = 2u * verification_calls + coverage_calls;
        if (prefix_driven) {
            if (verification_calls == 0u || count(shader_kind::seed_lookup_anchors) != 1u ||
                seed_groups > max_workgroup_count_x ||
                job.workgroups[static_cast<std::size_t>(shader_kind::seed_lookup_anchors)] != seed_groups ||
                count(shader_kind::prefix_scan_block) != verification_calls ||
                count(shader_kind::prefix_scan_add) != 2u * verification_calls) {
                return false;
            }
            ++expected_dispatches;
            ++expected_submissions;
            if (bitonic) {
                constexpr std::array required{
                    shader_kind::seed_lookup_anchors, shader_kind::sort_anchors_bitonic};
                if (!is_prepared_exactly(required, 0u, verification_calls, coverage_calls) ||
                    count(shader_kind::sort_anchors_bitonic) != 1u ||
                    job.workgroups[static_cast<std::size_t>(shader_kind::sort_anchors_bitonic)] != 1u)
                    return false;
                ++expected_dispatches;
            } else {
                constexpr std::array required{shader_kind::seed_lookup_anchors};
                if (!is_prepared_exactly(required, 1u, verification_calls, coverage_calls))
                    return false;
            }
        } else {
            constexpr std::array required{
                shader_kind::seed_count, shader_kind::seed_emit,
                shader_kind::prefix_scan_block, shader_kind::prefix_scan_add};
            if (bitonic || !is_prepared_exactly(required, verification_calls,
                                               verification_calls, coverage_calls) ||
                count(shader_kind::seed_count) != 1u ||
                count(shader_kind::prefix_scan_block) != 1u + verification_calls ||
                count(shader_kind::prefix_scan_add) != 2u + 2u * verification_calls ||
                count(shader_kind::seed_emit) != verification_calls ||
                job.workgroups[static_cast<std::size_t>(shader_kind::seed_count)] !=
                    std::min(seed_groups, static_cast<std::uint64_t>(max_workgroup_count_x)))
                return false;
            expected_dispatches += 4u + verification_calls;
            expected_submissions += 1u + verification_calls;
        }
        if (verification_calls != 0u && !bitonic) {
            const auto passes = runtime_grouped_soa_radix_pass_count(
                job.target_position_offset, job.target_len);
            if (!passes || count(shader_kind::radix_sort_histogram) != *passes ||
                count(shader_kind::radix_sort_scan) != *passes ||
                count(shader_kind::radix_sort_scatter) != *passes)
                return false;
            expected_dispatches += 3u * *passes;
        }
        if (job.anchor_count == 0u &&
            (job.anchor_digest_lo != k_seed_anchor_digest_seed_low ||
             job.anchor_digest_hi != k_seed_anchor_digest_seed_high))
            return false;
        std::uint64_t actual_dispatches = 0u;
        for (const auto dispatches : job.dispatches) {
            if (dispatches > (std::numeric_limits<std::uint64_t>::max)() - actual_dispatches)
                return false;
            actual_dispatches += dispatches;
        }
        return actual_dispatches == expected_dispatches &&
               job.committed_submissions == expected_submissions &&
               audit_.committed_submissions == expected_submissions;
    }
#endif

    /// Checks candidate generation, native verification and survivor ordering.
    /// Empty queries submit no work; a query with no candidates keeps its scan.
    [[nodiscard]] bool is_valid_for_grouped_soa_seed_anchor_roundtrip() const noexcept {
        constexpr std::array required{
            shader_kind::grouped_soa_seed_count, shader_kind::grouped_soa_seed_emit,
            shader_kind::prefix_scan_block, shader_kind::prefix_scan_add};
        if (!is_common_state_valid(true) || audit_.kind !=
                runtime_pipeline_operation_kind::grouped_soa_seed_anchor_roundtrip)
            return false;
        std::uint64_t common_authority = 0u;
        std::uint64_t candidate_jobs = 0u;
        std::uint64_t coverage_jobs = 0u;
        std::uint64_t active_jobs = 0u;
        for (const auto& job : audit_.jobs) {
            if (!job.shape_contract_set || job.binding_window_count == 0u || job.target_len == 0u ||
                job.range_count != 0u || job.query_authority_identity == 0u ||
                !job.anchor_order_checked || !job.anchor_order_accepted ||
                !job.has_valid_seed_native_verification())
                return false;
            if (common_authority == 0u)
                common_authority = job.query_authority_identity;
            else if (job.query_authority_identity != common_authority)
                return false;
            const auto passes = runtime_grouped_soa_radix_pass_count(
                job.target_position_offset, job.target_len);
            if (!passes)
                return false;
            const std::uint64_t verified = job.seed_candidate_count != 0u ? 1u : 0u;
            const std::uint64_t coverage = job.seed_coverage_interval_count != 0u ? 1u : 0u;
            if (job.anchor_count == 0u &&
                (job.anchor_digest_lo != k_seed_anchor_digest_seed_low ||
                 job.anchor_digest_hi != k_seed_anchor_digest_seed_high))
                return false;
            std::uint64_t expected_total = 0u;
            std::uint64_t expected_submissions = 0u;
            if (job.minimizer_count == 0u) {
                if (verified != 0u)
                    return false;
            } else {
                ++active_jobs;
                candidate_jobs += verified;
                coverage_jobs += coverage;
                expected_total = std::uint64_t{job.binding_window_count} + 3u +
                    verified * (job.binding_window_count + 3u * *passes + 5u) + coverage;
                expected_submissions = 1u + 3u * verified + coverage;
                if (job.count(shader_kind::grouped_soa_seed_count) != job.binding_window_count ||
                    job.count(shader_kind::prefix_scan_block) != 1u + verified ||
                    job.count(shader_kind::prefix_scan_add) != 2u + 2u * verified ||
                    job.count(shader_kind::grouped_soa_seed_emit) != verified * job.binding_window_count ||
                    job.count(shader_kind::radix_sort_histogram) != verified * *passes ||
                    job.count(shader_kind::radix_sort_scan) != verified * *passes ||
                    job.count(shader_kind::radix_sort_scatter) != verified * *passes)
                    return false;
            }
            std::uint64_t actual_total = 0u;
            for (const auto count : job.dispatches) {
                if (count > (std::numeric_limits<std::uint64_t>::max)() - actual_total)
                    return false;
                actual_total += count;
            }
            if (actual_total != expected_total || job.committed_submissions != expected_submissions)
                return false;
        }
        if ((active_jobs == 0u) != (audit_.committed_submissions == 0u))
            return false;
        return is_prepared_exactly(required, candidate_jobs, candidate_jobs, coverage_jobs);
    }

    /// Checks seeding, filtering, range construction and chaining separately.
    /// Sort work follows candidate counts; downstream work follows survivors.
    [[nodiscard]] bool is_valid_for_grouped_soa_coordinate() const noexcept {
        constexpr auto& coordinate_required = grouped_soa_coordinate_required_shaders;
        constexpr auto candidate_required = [] {
            auto required = grouped_soa_coordinate_required_shaders;
            std::ranges::replace(required, shader_kind::coordinate_reduce,
                                 shader_kind::chain_candidate_reduce);
            return required;
        }();
        if (!is_common_state_valid() ||
            audit_.kind != runtime_pipeline_operation_kind::grouped_soa_coordinate)
            return false;
        const bool candidate_mode = prepared_calls(shader_kind::chain_candidate_reduce) == 1u;
        if ((prepared_calls(shader_kind::coordinate_reduce) == 1u) == candidate_mode)
            return false;
        std::uint64_t candidate_jobs = 0u;
        std::uint64_t coverage_jobs = 0u;
        for (const auto& job : audit_.jobs) {
            if (!job.shape_contract_set || job.binding_window_count == 0u ||
                job.minimizer_count == 0u || !job.has_valid_seed_native_verification())
                return false;
            const auto passes = runtime_grouped_soa_radix_pass_count(
                job.target_position_offset, job.target_len);
            if (!passes)
                return false;
            const std::uint64_t verified = job.seed_candidate_count != 0u ? 1u : 0u;
            const std::uint64_t coverage = job.seed_coverage_interval_count != 0u ? 1u : 0u;
            const std::uint64_t ranged = job.anchor_count != 0u ? 1u : 0u;
            const std::uint64_t chained = job.range_count != 0u ? 1u : 0u;
            candidate_jobs += verified;
            coverage_jobs += coverage;
            if ((ranged == 0u && chained != 0u) ||
                job.count(shader_kind::grouped_soa_seed_count) != job.binding_window_count ||
                job.count(shader_kind::prefix_scan_block) != 1u + verified ||
                job.count(shader_kind::prefix_scan_add) != 2u + 2u * verified ||
                job.count(shader_kind::grouped_soa_seed_emit) != verified * job.binding_window_count ||
                job.count(shader_kind::radix_sort_histogram) != verified * *passes ||
                job.count(shader_kind::radix_sort_scan) != verified * *passes ||
                job.count(shader_kind::radix_sort_scatter) != verified * *passes ||
                job.count(shader_kind::chain_range_build) != ranged ||
                job.count(shader_kind::chain_tiled_dp_u64) != chained ||
                job.count(candidate_mode ? shader_kind::chain_candidate_reduce
                                         : shader_kind::coordinate_reduce) != chained)
                return false;
            const std::uint64_t expected_total = std::uint64_t{job.binding_window_count} + 3u +
                verified * (job.binding_window_count + 3u * *passes + 5u) +
                coverage + ranged + 2u * chained;
            const std::uint64_t expected_submissions =
                1u + 4u * verified + coverage + ranged + chained;
            std::uint64_t actual_total = 0u;
            for (const auto count : job.dispatches) {
                if (count > (std::numeric_limits<std::uint64_t>::max)() - actual_total)
                    return false;
                actual_total += count;
            }
            if (actual_total != expected_total || job.committed_submissions != expected_submissions)
                return false;
        }
        return is_prepared_exactly(candidate_mode ? std::span<const shader_kind>{candidate_required}
                                                  : std::span<const shader_kind>{coordinate_required},
                                   candidate_jobs, candidate_jobs, coverage_jobs);
    }

    /// @brief Checks completed alignment jobs for packed sequence windows.
    /// Each job must have one alignment dispatch, or a screening dispatch followed
    /// by a retry that produces the traceback.
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

    /// @brief Checks one completed dispatch that classifies alignments and computes MAPQ.
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

    /// @brief Tests accepted and rejected sequences of pipeline recording calls.
    /// @param[in] base_audit Valid Metal pipeline record used to build test receipts.
    /// @return Whether completion checks and one-time metric reporting behave as expected.
    // Define this after the collector so the test can call its private recording methods.
    [[nodiscard]] static bool
    self_test_causal_collector_contract(runtime_metal_archive_pipeline_audit base_audit);

  private:
    /// @brief Checks receipt ownership, GPU identity, and completed submission records.
    /// @param[in] allow_undispatched_jobs Allow jobs with  no submitted work.
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
                    if (job.submission_ordinals[i] == 0u ||
                        job.submission_ordinals[i] > audit_.committed_submissions ||
                        (i != 0u && job.submission_ordinals[i - 1u] >= job.submission_ordinals[i]))
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

    /// @brief Returns the prepare count for the first pipeline of this shader kind.
    /// Returns zero if no matching pipeline was prepared.
    [[nodiscard]] std::uint64_t prepared_calls(shader_kind kind) const noexcept {
        const std::uint32_t raw = static_cast<std::uint32_t>(kind);
        for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
            if (audit_.prepared[i].shader_key.kind == raw) {
                return audit_.prepared[i].prepared_calls;
            }
        }
        return 0u;
    }

    /// @brief Checks that the prepared pipelines match the operation's requirements.
    /// Base shaders are prepared once. Each candidate-bearing job retrieves its
    /// radix and native verification pipelines once; verification reuses the
    /// prefix shader keys and therefore adds their preparation counts.
    [[nodiscard]] bool is_prepared_exactly(std::span<const shader_kind> required,
                                           std::uint64_t radix_prepared_calls = 0u,
                                           std::uint64_t verification_calls = 0u,
                                           std::uint64_t coverage_calls = 0u) const noexcept {
        std::array<std::uint64_t, k_runtime_pipeline_kind_slots> expected{};
        for (const shader_kind kind : required) {
            const auto slot = static_cast<std::size_t>(kind);
            if (slot >= expected.size() || expected[slot] != 0u)
                return false;
            expected[slot] = 1u;
        }
        for (const shader_kind kind : seed_radix_required_shaders) {
            expected[static_cast<std::size_t>(kind)] = radix_prepared_calls;
        }
        expected[static_cast<std::size_t>(shader_kind::seed_verify_count)] = verification_calls;
        expected[static_cast<std::size_t>(shader_kind::seed_verify_emit)] = verification_calls;
        expected[static_cast<std::size_t>(shader_kind::seed_native_coverage)] = coverage_calls;
        for (const auto kind : {shader_kind::prefix_scan_block, shader_kind::prefix_scan_add}) {
            auto& count = expected[static_cast<std::size_t>(kind)];
            if (verification_calls > (std::numeric_limits<std::uint64_t>::max)() - count)
                return false;
            count += verification_calls;
        }
        for (std::size_t i = 0u; i < audit_.prepared_count; ++i) {
            const auto slot = static_cast<std::size_t>(audit_.prepared[i].shader_key.kind);
            if (slot >= expected.size() || expected[slot] == 0u ||
                audit_.prepared[i].prepared_calls != expected[slot])
                return false;
            expected[slot] = 0u;
        }
        return std::ranges::all_of(expected, [](const auto count) { return count == 0u; });
    }

    // Members are destroyed in reverse order Keep the reservation alive until
    // the jobs vector has released its storage.
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

/// @brief Tracks recorded dispatches until their GPU submissions complete.
class runtime_pipeline_use_collector {
  public:
    /// @brief Keeps a recording session from being copied.
    runtime_pipeline_use_collector(const runtime_pipeline_use_collector&) = delete;

    /// @brief Keeps assignment  from duplicating a recording session.
    runtime_pipeline_use_collector& operator=(const runtime_pipeline_use_collector&) = delete;

    /// @brief Keeps the collector at the address referenced by command recording.
    runtime_pipeline_use_collector(runtime_pipeline_use_collector&&) = delete;

    /// @brief Prevents replacing a collector while command recording refers to it.
    runtime_pipeline_use_collector& operator=(runtime_pipeline_use_collector&&) = delete;

    /// @brief Discards any dispatches still waiting for completion.
    ~runtime_pipeline_use_collector() {
        burn_pending();
    }

  private:
    /// @brief Dispatch and workgroup counts waiting for one submission to complete.
    struct pending_job {
        std::array<std::uint64_t, k_runtime_pipeline_kind_slots> dispatches{};
        std::array<std::uint64_t, k_runtime_pipeline_kind_slots> workgroups{};
    };

    /// @brief Reserves memory and creates a collector with a fixed number of jobs.
    /// `kind` and `operation_identity` identify the operation. The identity and
    /// `job_count` must be nonzero. Throws if the shape or memory reservation fails.
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

    /// @brief Allocates job records using an existing memory, reservation.
    /// The  caller has already checked the operation kind, ID, and job count.
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

    /// @brief Returns the collector's memory charge, including vector capacity and overhead.
    /// Returns the largest 64-bit value if the total overflows.
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

    /// @brief Shrinks the reservation to the memory held by the collector.
    /// Throws if the allocated capacity exceeded the reservation.
    void reconcile_capacity_permit() {
        const std::uint64_t retained = retained_capacity_bytes();
        if (retained == (std::numeric_limits<std::uint64_t>::max)() || !capacity_permit_ ||
            retained > capacity_permit_.bytes()) {
            throw vulkan_runtime_error(
                "runtime pipeline receipt collector exceeded admitted capacity");
        }
        capacity_permit_.shrink_to(retained);
    }

    /// @brief Adds to a counter and reports whether the lddition fit.
    /// On overflow, leaves the counter at the largest 64-bit value and returns false.
    static bool add_counter(std::uint64_t& destination, std::uint64_t value) noexcept {
        if (value > (std::numeric_limits<std::uint64_t>::max)() - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            return false;
        }
        destination += value;
        return true;
    }

    /// @brief Makes this operation fail its completion checks.
    void invalidate() noexcept {
        audit_.invalid = true;
    }

    /// @brief Registers a  pipeline before its dispatches are recorded.
    /// The first receipt supplies the GPU and cache identity Later receipts must
    /// match it, and repeated preparation of a shader must use the same receipt.
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
        // Leave reporting flags unchanged until the final runtime check succeeds.
        // If this operation fails, the next accepted operation can report the
        // seed load and pipeline creation costs. The finalizer updates those
        // flags while holding runtime::mu_.
    }

    /// @brief Records input sizes for one job.
    /// Seed jobs use minimizer count, target length and  offset, and the number
    /// of separately bound target windows. Alignment and MAPQ reuse these fields
    /// for the sizes and identities checked by their operation validators.
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

    /// @brief Records output sizes for a job whose input has been recorded.
    /// Seed and coordinate jobs store anchor and chain-rangm counts. Other
    /// operation kinds assign these fields in their own completion checks.
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

    /// Records completed native verification after both fences. Counts refer to
    /// raw hash candidates, accepted native spans, and actually compacted anchors.
    void set_seed_native_verification(std::size_t job_index,
                                      std::uint64_t candidate_count,
                                      std::uint64_t verified_count,
                                      std::uint64_t emitted_count,
                                      std::uint64_t coverage_interval_count) noexcept {
        if (job_index >= audit_.jobs.size() || !audit_.jobs[job_index].shape_contract_set ||
            audit_.jobs[job_index].seed_native_verification_set || candidate_count == 0u ||
            candidate_count > (std::numeric_limits<std::uint32_t>::max)() ||
            verified_count > candidate_count || emitted_count != verified_count ||
            (coverage_interval_count != 0u && coverage_interval_count != candidate_count)) {
            invalidate();
            return;
        }
        auto& job = audit_.jobs[job_index];
        job.seed_candidate_count = candidate_count;
        job.seed_verified_count = verified_count;
        job.seed_emitted_count = emitted_count;
        job.seed_coverage_interval_count = coverage_interval_count;
        job.seed_native_verification_set = true;
    }

    /// @brief Attaches the query authorization ID and the host check of anchor order.
    /// The order check must cover exactly this job's recorded anchor  count.
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

    /// @brief Starts recording dispatches for this use of a command buffer.
    /// Starting while a previous recording is, active invalidates the operation.
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

    /// @brief Records a dispatch for a prepared pipeline and job.
    /// The command buffer and receipt must match the active recording A nonzero
    /// `observed_generation` also checks that the caller refers to this recording.
    /// `group_count` must be nonzero. Counts remain pending until `commit`.
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
    /// @brief Counts the pending dispatches as  part of a completed submission.
    /// The caller must finish submission checks first. Each affected job records
    /// the same submission ordinal, so shared submissions can be counted once.
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

    /// @brief Discards pending dispatches and records how many were abandoned.
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

    /// @brief Discards the ctive recording before reset and invalidates the operation.
    void burn_before_reset(VkCommandBuffer command_buffer) noexcept {
        if (!active_)
            return;
        if (vulkan_handle_identity(command_buffer) != command_identity_) {
            invalidate();
        }
        burn_pending();
        // Resetting an unfinished recording invalidates the operation even if it
        // contains no dispatches, in which case the discarded-use count is zero.
        invalidate();
    }

  private:
    /// @brief Moves the operation record and its memory reservation into a token.
    /// An unfinished  command recording makes the resulting token invalid.
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
        // Free pending dispatch storage before reducing the reservation. Keep
        // enough reserved for the collector until its owner destroys it. The
        // token keeps this small extra charge for the lifetime of its jobs vector.
        {
            std::vector<pending_job> empty;
            pending_.swap(empty);
        }
        {
            // Free any storage left in the moved-from jobs vector before transferring
            // its memory reservation to the token.
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

    // Destroy the vectors before releasing their memory reservation
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
    const std::array<shader_variant_key, 16> keys{
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
        make_shader_key(shader_kind::seed_verify_count, 384u, 128u, 1u),
        make_shader_key(shader_kind::seed_verify_emit, 384u, 128u, 1u),
        make_shader_key(shader_kind::seed_native_coverage, 384u, 128u, 1u),
    };
    if (!std::all_of(keys.begin(), keys.end(), [](const shader_variant_key& key) {
            return is_shader_variant_key_valid(key);
        })) {
        return false;
    }
    std::array<runtime_metal_archive_pipeline_receipt, 16> receipts{};
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
    // Use fixed non-null handles to test recording  without creating Vulkan objects.
    const auto command = [](std::uintptr_t identity) {
        return reinterpret_cast<VkCommandBuffer>(identity);
    };
    // Completes the same per-job native protocol as the runtime verifier.
    const auto record_verification = [&keys, &receipts, command](
        runtime_pipeline_use_collector& collector, std::size_t job,
        std::uint64_t candidate_count, std::uint64_t survivor_count,
        bool coverage, std::uintptr_t command_base) {
        if (coverage) {
            collector.prepare(keys[15], &receipts[15]);
            collector.begin(command(command_base));
            collector.use(command(command_base), job, keys[15], &receipts[15], 1u);
            collector.commit(command(command_base));
        }
        for (const auto slot : {13u, 14u, 1u, 2u})
            collector.prepare(keys[slot], &receipts[slot]);
        collector.begin(command(command_base + 1u));
        collector.use(command(command_base + 1u), job, keys[13], &receipts[13], 1u);
        collector.use(command(command_base + 1u), job, keys[1], &receipts[1], 1u);
        collector.use(command(command_base + 1u), job, keys[2], &receipts[2], 1u);
        collector.use(command(command_base + 1u), job, keys[2], &receipts[2], 1u);
        collector.commit(command(command_base + 1u));
        collector.begin(command(command_base + 2u));
        collector.use(command(command_base + 2u), job, keys[14], &receipts[14], 1u);
        collector.commit(command(command_base + 2u));
        collector.set_seed_native_verification(job, candidate_count, survivor_count,
                                              survivor_count, coverage ? candidate_count : 0u);
    };
    // Registers the three pipelines required by query sketching.
    const auto prepare_query = [&keys, &receipts](runtime_pipeline_use_collector& collector) {
        for (std::size_t i = 0u; i < 3u; ++i) {
            collector.prepare(keys[i], &receipts[i]);
        }
    };
    // Builds the expected query receipt with both submissions completed.
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

    // Each, mode introduces a recording error that the query validator must reject.
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
        // Each job with anchors retrieves the radix pipelines onue, then reuses
        // them for every byte pass.
        for (std::size_t repeat = 0u; repeat < 2u; ++repeat) {
            for (std::size_t slot = 8u; slot < 11u; ++slot) {
                collector->prepare(keys[slot], &receipts[slot]);
            }
        }
        collector->set_shape_input(0u, 1u, 1u, 0u);
        collector->set_shape_input(1u, 1u, 255u, 0u);
        collector->set_shape_input(2u, 1u, 65536u, 0u, 3u);
        collector->set_shape_outcome(0u, 0u, 0u);
        collector->set_shape_outcome(1u, 0u, 0u);
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
        collector->commit(command(13u));
        record_verification(*collector, 1u, 1u, 0u, false, 20u);
        record_verification(*collector, 2u, 1u, 1u, true, 30u);
        collector->begin(command(40u));
        collector->use(command(40u), 2u, keys[5], &receipts[5], 1u);
        collector->commit(command(40u));

        collector->begin(command(14u));
        collector->use(command(14u), 2u, keys[7], &receipts[7], 1u);
        collector->use(command(14u), 2u, keys[6], &receipts[6], 1u);
        collector->commit(command(14u));
        shape = collector->seal();
    } catch (...) {
        return false;
    }
    if (!shape.is_valid_for_grouped_soa_coordinate() ||
        shape.audit_.begun_command_generations != 10u ||
        shape.audit_.committed_command_generations != 10u || !shape.capacity_permit_) {
        return false;
    }
    for (const auto kind : {shader_kind::grouped_soa_seed_count, shader_kind::grouped_soa_seed_emit}) {
        auto& count = shape.audit_.jobs[2].dispatches[static_cast<std::size_t>(kind)];
        --count;
        if (shape.is_valid_for_grouped_soa_coordinate()) return false;
        ++count;
    }
    constexpr std::array<std::uint64_t, 3> expected_submissions{1u, 5u, 8u};
    for (std::size_t job = 0u; job < shape.audit_.jobs.size(); ++job) {
        const runtime_pipeline_job_use_audit& audit = shape.audit_.jobs[job];
        if (audit.submission_operation_identity != 202u ||
            audit.committed_submissions != expected_submissions[job] ||
            audit.count(shader_kind::coordinate_vote_reduce) != 0u ||
            audit.count(shader_kind::coordinate_vote_reduce_batch) != 0u) {
            return false;
        }
        for (std::size_t ordinal = 0u; ordinal < audit.committed_submissions; ++ordinal) {
            if (audit.submission_ordinals[ordinal] == 0u ||
                (ordinal != 0u && audit.submission_ordinals[ordinal - 1u] >=
                                      audit.submission_ordinals[ordinal])) {
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

    // The stored-index seed test ends after anchor sorting. An empty query
    // still records its authorization and empty result without submitting GPU work.
    runtime_pipeline_operation_token seed_only{};
    runtime_pipeline_operation_token rejected_seed_only{};
    runtime_pipeline_operation_token empty_seed_only{};
    try {
        const auto make_seed_only = [&keys, &receipts, &record_verification, command](std::uint64_t identity,
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
            record_verification(*collector, 2u, 1u, 1u, false,
                                static_cast<std::uintptr_t>(identity + 10u));
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
        seed_only.audit_.committed_submissions != 4u ||
        seed_only.audit_.jobs[0].committed_submissions != 0u ||
        seed_only.audit_.jobs[1].committed_submissions != 1u ||
        seed_only.audit_.jobs[2].committed_submissions != 4u ||
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

    // Alignment may screen jobs first and run traceback only for the survivors.
    // Count each submission once for the operation and once for every job it touches.
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

    // MAPQ has its own submission. Input fields record  problem, candidate, and
    // alignment counts. Output fields record the published count and result digest.
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
    // Check that the chaining receipt rejects missing work, unfinished
    // submissions, and an extra coordinate-reduction dispatch.
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

    // These synthetic receipts test both MPF seed routes, including a GPU count
    // that finds no anchors. They exercise validation without dispatching GPU work.
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
        const auto make_mpf = [&extra_keys, &extra_receipts, &keys, &receipts, &record_verification, command](
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
            if (!empty)
                record_verification(*collector, 0u, 1u, 1u, false,
                                    static_cast<std::uintptr_t>(identity + 20'000u));
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

    // Preparing or abandoning an operation must leave its costs available to
    // report later. Abandon one claim plan to model a failed final check, then
    // verify  that the next commit reports all costs and a later commit reports zero
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

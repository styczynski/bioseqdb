/**
 * @file coordinates.cppm
 * @brief Defines seed-to-coordinate inputs, audits, receipts, and results.
 */
module;

#include <cstddef>
#include <cstdint>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:frontend_types.coordinates;

import :receipts;
import :frontend_types.sketch;
import :frontend_types.arena;
import :frontend_types.indexing;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import std;

export namespace bioseq::gpu::vulkan {

/// @brief Common query-side inputs for a batched resident seed→chain→coordinate run.
/// The query minimizer words are shared by every job in the batch. Source-direct jobs
/// may supply distinct slot-table targets. An authenticated grouped-SoA direct
/// operation is intentionally restricted to one resident generation/view.
struct gpu_seed_query_inputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const buffer* device_minimizer_words = nullptr;
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t window_size = 0;
    std::span<const gpu_seed_query_record_wire> host_query_records{};
    const buffer* device_query_records = nullptr;
};

/// @brief Neutral resident view of one authenticated persisted grouped-SoA generation.
/// The owner keeps all three buffers alive for the complete batch.  Byte sizes
/// are logical admitted sizes. The backing raw control/position buffers may be
/// padded to whole uint32 words for GLSL access, but their representation is
/// never decoded or rebuilt by this runtime.
struct gpu_grouped_soa_resident_view {
    const buffer* descriptors = nullptr;
    const buffer* controls = nullptr;
    const buffer* positions = nullptr;
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_revision = 0;
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
};

/// @brief Mutually exclusive resident target-index representations accepted by seeding.
enum class gpu_seed_index_representation : std::uint8_t {
    invalid = 0,
    source_direct_sorted_dense = 1,
    grouped_soa = 2,
};

/// @brief One resident target-index job in a batched seed→coordinate run. Source-direct
/// slot-table fields and `grouped_soa` are an XOR: selecting both (or neither) is
/// invalid rather than permission to fall back to another implementation.
struct gpu_seed_coordinate_job {
    const buffer* slots = nullptr;
    const buffer* positions = nullptr;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t bucket_count = 0;
    std::uint64_t target_position_offset = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    const gpu_grouped_soa_resident_view* grouped_soa = nullptr;
};

[[nodiscard]] inline constexpr gpu_seed_index_representation
classify_gpu_seed_index_representation(const gpu_seed_coordinate_job& job) noexcept {
    const bool grouped_soa_only = job.grouped_soa != nullptr && job.slots == nullptr &&
                                  job.positions == nullptr && job.slot_mask == 0u &&
                                  job.position_count == 0u && job.bucket_count == 0u &&
                                  job.target_position_offset == 0u;
    if (grouped_soa_only)
        return gpu_seed_index_representation::grouped_soa;

    const bool source_direct_only =
        job.grouped_soa == nullptr && job.slots != nullptr && job.positions != nullptr;
    return source_direct_only ? gpu_seed_index_representation::source_direct_sorted_dense
                              : gpu_seed_index_representation::invalid;
}

/// @brief Explicit algorithm identity for the seed-to-coordinate backend.  A caller
/// selecting Vulkan tiled-DP must pass `vulkan_tiled_dp`. The approximate
/// coordinate-vote prototype is a distinct algorithm and may never be entered
/// implicitly as an optimization of tiled-DP.
enum class gpu_seed_coordinate_algorithm : std::uint8_t {
    vulkan_tiled_dp = 0,
    coordinate_vote = 1,
};

/// @brief Exact algorithm inputs bound into a persisted grouped-SoA coordinate receipt.
/// Floating-point penalties are carried by bit pattern so NaNs, signed zero,
/// and every other dispatch-affecting value compare without normalization.
struct persisted_grouped_soa_coordinate_config_identity {
    std::uint32_t kmer_size = 0u;
    std::int32_t max_occ = -1;
    gpu_seed_coordinate_algorithm coordinate_algorithm =
        gpu_seed_coordinate_algorithm::vulkan_tiled_dp;
    chaining_pipeline_key chain_key{};
    std::int32_t max_dist_x = 0;
    std::int32_t max_dist_y = 0;
    std::int32_t bandwidth = 0;
    std::int32_t max_iter = 0;
    std::uint32_t gap_penalty_bits = 0u;
    std::uint32_t skip_penalty_bits = 0u;
    std::int32_t max_chain_skip = 0;
    std::int32_t min_chain_score = 0;
    std::int32_t min_seed_count = 0;
    std::uint32_t probe_limit = 0u;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return kmer_size != 0u && kmer_size <= 255u && max_occ >= 0 &&
               coordinate_algorithm == gpu_seed_coordinate_algorithm::vulkan_tiled_dp &&
               chain_key.tile_size != 0u && chain_key.local_size != 0u && probe_limit != 0u;
    }
    [[nodiscard]] bool
    operator==(const persisted_grouped_soa_coordinate_config_identity&) const = default;
};

[[nodiscard]] inline persisted_grouped_soa_coordinate_config_identity
make_persisted_grouped_soa_coordinate_config_identity(
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    gpu_seed_coordinate_algorithm coordinate_algorithm,
    const chaining_pipeline_key& chain_key,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit) noexcept {
    return persisted_grouped_soa_coordinate_config_identity{
        .kmer_size = kmer_size,
        .max_occ = max_occ,
        .coordinate_algorithm = coordinate_algorithm,
        .chain_key = chain_key,
        .max_dist_x = max_dist_x,
        .max_dist_y = max_dist_y,
        .bandwidth = bandwidth,
        .max_iter = max_iter,
        .gap_penalty_bits = std::bit_cast<std::uint32_t>(gap_penalty),
        .skip_penalty_bits = std::bit_cast<std::uint32_t>(skip_penalty),
        .max_chain_skip = max_chain_skip,
        .min_chain_score = min_chain_score,
        .min_seed_count = min_seed_count,
        .probe_limit = probe_limit,
    };
}

static_assert(!persisted_grouped_soa_coordinate_config_identity{}.is_valid());

/// @brief Dispatch counts emitted by one seed-to-coordinate batch.
struct gpu_seed_coordinate_batch_stats {
    std::uint64_t problem_count = 0;
    std::uint64_t wave_count = 0;
    std::uint64_t seed_count_dispatches = 0;
    std::uint64_t seed_emit_dispatches = 0;
    std::uint64_t sort_dispatches = 0;
    std::uint64_t range_build_dispatches = 0;
    std::uint64_t chain_dispatches = 0;
    std::uint64_t chain_backtrack_dispatches = 0;
    std::uint64_t coordinate_vote_dispatches = 0;
};

[[nodiscard]] inline constexpr bool
are_coordinate_result_wires_equal(const gpu_coordinate_result_wire& lhs,
                                  const gpu_coordinate_result_wire& rhs) noexcept {
    return lhs.query_start == rhs.query_start && lhs.query_end == rhs.query_end &&
           lhs.target_start == rhs.target_start && lhs.target_end == rhs.target_end &&
           lhs.match_length == rhs.match_length && lhs.block_length == rhs.block_length &&
           lhs.chain_score == rhs.chain_score && lhs.seed_count == rhs.seed_count &&
           lhs.is_reverse == rhs.is_reverse && lhs.is_valid == rhs.is_valid;
}

[[nodiscard]] inline constexpr bool
is_coordinate_result_wire_well_formed(const gpu_coordinate_result_wire& result,
                                      const std::uint64_t query_len,
                                      const std::uint64_t target_len) noexcept {
    if (result.is_valid == 0u) {
        return are_coordinate_result_wires_equal(result, gpu_coordinate_result_wire{});
    }
    if (result.is_valid != 1u || result.is_reverse > 1u || result.seed_count <= 0 ||
        result.query_start >= result.query_end || result.query_end > query_len ||
        result.target_start >= result.target_end || result.target_end > target_len ||
        result.match_length > result.block_length) {
        return false;
    }
    const std::uint64_t query_span = result.query_end - result.query_start;
    const std::uint64_t target_span = result.target_end - result.target_start;
    return result.match_length <= query_span && result.match_length <= target_span &&
           result.block_length <= query_span + target_span;
}

/// @brief Exact caller-side identities checked while consuming one sealed operation
/// record.  These are expectations, not receipt: changing any value makes the
/// one-shot consume fail.
struct persisted_grouped_soa_coordinate_job_expectation {
    std::size_t problem_index = 0;
    std::uint64_t query_authority_identity = 0;
    runtime_buffer_identity resident_descriptors{};
    runtime_buffer_identity resident_controls{};
    runtime_buffer_identity resident_positions{};
    runtime_buffer_identity query_minimizer_buffer{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_revision = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint64_t target_len = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    gpu_seed_query_record_wire query_record{};
    gpu_coordinate_result_wire result{};
    persisted_grouped_soa_coordinate_config_identity configuration{};
};

/// @brief Operation-local audit payload.  runtime fills this from successful Vulkan
/// calls. It is public only so the contract can be golden/tamper tested. It
/// cannot be converted into an operation receipt by callers.
struct persisted_grouped_soa_coordinate_job_audit {
    std::uint64_t operation_identity = 0;
    std::uint64_t query_authority_identity = 0;
    runtime_buffer_identity resident_descriptors{};
    runtime_buffer_identity resident_controls{};
    runtime_buffer_identity resident_positions{};
    runtime_buffer_identity query_minimizer_buffer{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_revision = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint64_t target_len = 0;
    std::size_t problem_index = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    gpu_seed_query_record_wire query_record{};
    gpu_coordinate_result_wire result{};
    persisted_grouped_soa_coordinate_config_identity configuration{};
    std::uint32_t query_tile_count = 0u;
    std::uint64_t query_count_dispatches = 0;
    std::uint64_t query_scan_dispatches = 0;
    std::uint64_t query_emit_dispatches = 0;
    std::uint64_t query_completed_roundtrips = 0;
    std::uint64_t direct_count_dispatches = 0;
    std::uint64_t direct_scan_dispatches = 0;
    std::uint64_t direct_emit_dispatches = 0;
    std::uint64_t anchor_count = 0;
    std::uint64_t sort_operations = 0;
    std::uint64_t range_build_dispatches = 0;
    std::uint64_t range_count = 0;
    std::uint64_t chain_dispatches = 0;
    std::uint64_t chain_backtrack_dispatches = 0;
    std::uint64_t coordinate_dispatches = 0;
    std::uint64_t count_submit_wait_identity = 0;
    std::uint64_t emit_submit_wait_identity = 0;
    std::uint64_t sort_range_submit_wait_identity = 0;
    std::uint64_t chain_coordinate_submit_wait_identity = 0;
    std::uint64_t count_status_checks = 0;
    std::uint64_t emit_status_checks = 0;
    std::uint64_t result_readbacks = 0;
    std::uint32_t status_or = 0;
    std::uint64_t coordinate_vote_dispatches = 0;
    std::uint64_t cpu_or_simd_biological_calls = 0;
    std::uint64_t recompute_calls = 0;
    std::uint64_t fallback_calls = 0;

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        const bool identity_and_query =
            operation_identity != 0u && query_authority_identity != 0u &&
            resident_descriptors.is_valid() && resident_controls.is_valid() &&
            resident_positions.is_valid() && query_minimizer_buffer.is_valid() &&
            resident_descriptors.object != resident_controls.object &&
            resident_descriptors.object != resident_positions.object &&
            resident_controls.object != resident_positions.object &&
            query_minimizer_buffer.object != resident_descriptors.object &&
            query_minimizer_buffer.object != resident_controls.object &&
            query_minimizer_buffer.object != resident_positions.object &&
            resident_descriptors.buffer_handle != resident_controls.buffer_handle &&
            resident_descriptors.buffer_handle != resident_positions.buffer_handle &&
            resident_controls.buffer_handle != resident_positions.buffer_handle &&
            query_minimizer_buffer.buffer_handle != resident_descriptors.buffer_handle &&
            query_minimizer_buffer.buffer_handle != resident_controls.buffer_handle &&
            query_minimizer_buffer.buffer_handle != resident_positions.buffer_handle &&
            resident_descriptors.memory_handle != resident_controls.memory_handle &&
            resident_descriptors.memory_handle != resident_positions.memory_handle &&
            resident_controls.memory_handle != resident_positions.memory_handle &&
            query_minimizer_buffer.memory_handle != resident_descriptors.memory_handle &&
            query_minimizer_buffer.memory_handle != resident_controls.memory_handle &&
            query_minimizer_buffer.memory_handle != resident_positions.memory_handle &&
            device_uuid != std::array<std::uint8_t, VK_UUID_SIZE>{} && descriptor_bytes != 0u &&
            descriptor_bytes <= resident_descriptors.bytes && control_bytes != 0u &&
            control_bytes <= resident_controls.bytes && position_bytes != 0u &&
            position_bytes <= resident_positions.bytes && position_count != 0u &&
            max_occ_cap != (std::numeric_limits<std::uint64_t>::max)() && partition_count != 0u &&
            descriptor_bytes == static_cast<std::uint64_t>(partition_count) *
                                    sizeof(gpu_grouped_soa_resident_partition_descriptor) &&
            abi_revision == k_gpu_grouped_soa_abi_revision && generation_id != 0u &&
            source_identity != 0u && cold_load_identity != 0u && target_len != 0u &&
            query_record.flags == 0u && configuration.is_valid() &&
            static_cast<std::uint64_t>(configuration.max_occ) <= max_occ_cap &&
            query_tile_count != 0u && query_count_dispatches == 1u && query_scan_dispatches == 3u &&
            query_emit_dispatches == 1u && query_completed_roundtrips == 2u && status_or == 0u &&
            coordinate_vote_dispatches == 0u && cpu_or_simd_biological_calls == 0u &&
            recompute_calls == 0u && fallback_calls == 0u;
        if (!identity_and_query)
            return false;
        if (query_record.minimizer_count == 0u) {
            // The canonical persisted-GPU contract requires a genuine
            // per-job partition-selection/seed-lookup dispatch. A query that
            // produced no minimizers cannot supply those operation statistics,
            // so it remains explicitly unsupported rather than being
            // relabelled from batch-level sketch work.
            return false;
        }
        const bool direct_front_end = direct_count_dispatches == 1u &&
                                      direct_scan_dispatches == 3u &&
                                      count_submit_wait_identity != 0u && count_status_checks == 1u;
        if (!direct_front_end)
            return false;
        if (anchor_count == 0u) {
            return direct_emit_dispatches == 0u && sort_operations == 0u &&
                   range_build_dispatches == 0u && range_count == 0u && chain_dispatches == 0u &&
                   chain_backtrack_dispatches == 0u && coordinate_dispatches == 0u &&
                   emit_submit_wait_identity == 0u && sort_range_submit_wait_identity == 0u &&
                   chain_coordinate_submit_wait_identity == 0u && emit_status_checks == 0u &&
                   result_readbacks == 0u &&
                   are_coordinate_result_wires_equal(result, gpu_coordinate_result_wire{});
        }
        const bool emitted_and_ranged =
            direct_emit_dispatches == 1u && sort_operations == 1u && range_build_dispatches == 1u &&
            emit_submit_wait_identity != 0u && sort_range_submit_wait_identity != 0u &&
            count_submit_wait_identity != emit_submit_wait_identity &&
            count_submit_wait_identity != sort_range_submit_wait_identity &&
            emit_submit_wait_identity != sort_range_submit_wait_identity &&
            emit_status_checks == 1u;
        if (!emitted_and_ranged)
            return false;
        if (range_count == 0u) {
            return chain_dispatches == 0u && chain_backtrack_dispatches == 0u &&
                   coordinate_dispatches == 0u && chain_coordinate_submit_wait_identity == 0u &&
                   result_readbacks == 0u &&
                   are_coordinate_result_wires_equal(result, gpu_coordinate_result_wire{});
        }
        return chain_dispatches == 1u && chain_backtrack_dispatches == 1u &&
               coordinate_dispatches == 1u && chain_coordinate_submit_wait_identity != 0u &&
               chain_coordinate_submit_wait_identity != sort_range_submit_wait_identity &&
               chain_coordinate_submit_wait_identity != emit_submit_wait_identity &&
               chain_coordinate_submit_wait_identity != count_submit_wait_identity &&
               result_readbacks == 1u &&
               is_coordinate_result_wire_well_formed(result, query_record.query_len, target_len);
    }
};

/// @brief Unforgeable consumed view of one accepted audit record.
class persisted_grouped_soa_coordinate_job_receipt {
  public:
    persisted_grouped_soa_coordinate_job_receipt() = default;
    persisted_grouped_soa_coordinate_job_receipt(
        const persisted_grouped_soa_coordinate_job_receipt&) = delete;
    persisted_grouped_soa_coordinate_job_receipt&
    operator=(const persisted_grouped_soa_coordinate_job_receipt&) = delete;
    persisted_grouped_soa_coordinate_job_receipt(
        persisted_grouped_soa_coordinate_job_receipt&& other) noexcept {
        move_from(other);
    }
    persisted_grouped_soa_coordinate_job_receipt&
    operator=(persisted_grouped_soa_coordinate_job_receipt&& other) noexcept {
        if (this != &other) {
            audit_ = {};
            pipeline_operation_identity_ = 0u;
            issued_ = false;
            move_from(other);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && pipeline_operation_identity_ != 0u && audit_.is_internally_consistent();
    }
    [[nodiscard]] const persisted_grouped_soa_coordinate_job_audit& audit() const noexcept {
        return audit_;
    }

  private:
    friend class persisted_grouped_soa_coordinate_operation_receipt;

    void move_from(persisted_grouped_soa_coordinate_job_receipt& other) noexcept {
        audit_ = other.audit_;
        issued_ = other.issued_;
        pipeline_operation_identity_ = other.pipeline_operation_identity_;
        other.audit_ = {};
        other.issued_ = false;
        other.pipeline_operation_identity_ = 0u;
    }

    persisted_grouped_soa_coordinate_job_audit audit_{};
    std::uint64_t pipeline_operation_identity_ = 0u;
    bool issued_ = false;
};

/// @brief Aggregate acceptance accounting for a grouped-SoA coordinate operation.
struct persisted_grouped_soa_coordinate_operation_acceptance_audit {
    std::uint64_t operation_identity = 0u;
    std::uint64_t expected_job_count = 0u;
    std::uint64_t consumed_count = 0u;
    std::uint64_t accepted_count = 0u;
    std::uint64_t burned_count = 0u;
    std::uint64_t query_pipeline_receipt_count = 0u;
    std::uint64_t shape_pipeline_receipt_count = 0u;
    std::uint64_t committed_submission_count = 0u;
    std::uint64_t seed_receipt_identity = 0u;

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        return operation_identity != 0u && expected_job_count != 0u &&
               consumed_count == expected_job_count && accepted_count == expected_job_count &&
               burned_count == 0u && query_pipeline_receipt_count == 3u &&
               shape_pipeline_receipt_count >= 7u && shape_pipeline_receipt_count <= 10u &&
               committed_submission_count >= 3u && seed_receipt_identity != 0u;
    }
    [[nodiscard]] constexpr bool
    operator==(const persisted_grouped_soa_coordinate_operation_acceptance_audit&) const noexcept =
        default;
};

/**
 * @brief Allocation-free source for one persisted GPU operation result.
 *
 * `acceptance` supplies operation and receipt counts. The remaining fields fill
 * `persisted_gpu_operation_stats` query and shape identities, query submission
 * ordinals, environment, cache qualification, and cache metrics.
 */
struct persisted_grouped_soa_coordinate_operation_receipt_preview {
    persisted_grouped_soa_coordinate_operation_acceptance_audit acceptance{};
    std::uint64_t query_pipeline_operation_identity = 0u;
    std::uint64_t shape_pipeline_operation_identity = 0u;
    std::array<std::uint64_t, 2> query_submission_ordinals{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    runtime_metal_archive_process_qualification cache_qualification{};
    runtime_gpu_cache_metrics_audit maximum_cache_metrics{};

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return acceptance.is_internally_consistent() && query_pipeline_operation_identity != 0u &&
               shape_pipeline_operation_identity == acceptance.operation_identity &&
               query_pipeline_operation_identity != shape_pipeline_operation_identity &&
               query_submission_ordinals[0] != 0u && query_submission_ordinals[1] != 0u &&
               query_submission_ordinals[0] != query_submission_ordinals[1] && api_version != 0u &&
               driver_version != 0u && seed_size != 0u && manifest_checksum != 0u &&
               cache_qualification.is_internally_consistent() &&
               cache_qualification.vulkan_device_uuid == device_uuid &&
               cache_qualification.api_version == api_version &&
               cache_qualification.driver_version == driver_version &&
               cache_qualification.vendor_id == vendor_id &&
               cache_qualification.device_id == device_id &&
               cache_qualification.pipeline_cache_uuid == pipeline_cache_uuid &&
               cache_qualification.seed_sha256 == seed_sha256 &&
               cache_qualification.seed_size == seed_size &&
               cache_qualification.manifest_checksum == manifest_checksum &&
               maximum_cache_metrics.is_internally_consistent() &&
               maximum_cache_metrics.token_identity == acceptance.operation_identity &&
               std::any_of(device_uuid.begin(),
                           device_uuid.end(),
                           [](const std::uint8_t value) { return value != 0u; }) &&
               std::any_of(pipeline_cache_uuid.begin(),
                           pipeline_cache_uuid.end(),
                           [](const std::uint8_t value) { return value != 0u; }) &&
               std::any_of(seed_sha256.begin(), seed_sha256.end(), [](const std::uint8_t value) {
                   return value != 0u;
               });
    }
    [[nodiscard]] constexpr bool operator==(
        const persisted_grouped_soa_coordinate_operation_receipt_preview&) const noexcept = default;
};

/// @brief Exact full-stage job payload plus its structured Shape submission receipts.
struct persisted_grouped_soa_coordinate_job_receipt_preview {
    persisted_grouped_soa_coordinate_job_audit audit{};
    std::uint64_t shape_pipeline_operation_identity = 0u;
    std::array<std::uint64_t, 4> shape_submission_ordinals{};

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        if (!audit.is_internally_consistent() || audit.anchor_count == 0u ||
            audit.range_count == 0u || audit.result.is_valid != 1u ||
            shape_pipeline_operation_identity == 0u) {
            return false;
        }
        for (std::size_t i = 0u; i < shape_submission_ordinals.size(); ++i) {
            if (shape_submission_ordinals[i] == 0u)
                return false;
            for (std::size_t prior = 0u; prior < i; ++prior) {
                if (shape_submission_ordinals[prior] == shape_submission_ordinals[i]) {
                    return false;
                }
            }
        }
        return true;
    }
};

/// @brief Move-only accepted operation and its immutable environment identity.
class persisted_grouped_soa_coordinate_accepted_operation {
  public:
    persisted_grouped_soa_coordinate_accepted_operation() = default;
    persisted_grouped_soa_coordinate_accepted_operation(
        const persisted_grouped_soa_coordinate_accepted_operation&) = delete;
    persisted_grouped_soa_coordinate_accepted_operation&
    operator=(const persisted_grouped_soa_coordinate_accepted_operation&) = delete;
    persisted_grouped_soa_coordinate_accepted_operation(
        persisted_grouped_soa_coordinate_accepted_operation&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          environment_(std::exchange(other.environment_, {})),
          cache_metrics_(std::move(other.cache_metrics_)),
          issued_(std::exchange(other.issued_, false)) {}
    persisted_grouped_soa_coordinate_accepted_operation&
    operator=(persisted_grouped_soa_coordinate_accepted_operation&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            environment_ = std::exchange(other.environment_, {});
            cache_metrics_ = std::move(other.cache_metrics_);
            issued_ = std::exchange(other.issued_, false);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && audit_.is_internally_consistent() &&
               environment_.is_internally_consistent() && cache_metrics_.is_valid();
    }
    [[nodiscard]] const persisted_grouped_soa_coordinate_operation_acceptance_audit&
    audit() const noexcept {
        return audit_;
    }
    [[nodiscard]] const runtime_gpu_execution_environment_snapshot&
    environment_binding() const noexcept {
        return environment_;
    }
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& cache_metrics_audit() const noexcept {
        return cache_metrics_.audit();
    }
    [[nodiscard]] runtime_gpu_cache_metrics_delta take_cache_metrics_delta() noexcept {
        return std::move(cache_metrics_);
    }

  private:
    persisted_grouped_soa_coordinate_operation_acceptance_audit audit_{};
    runtime_gpu_execution_environment_snapshot environment_{};
    runtime_gpu_cache_metrics_delta cache_metrics_{};
    bool issued_ = false;
    friend class runtime;
};

static_assert(!std::is_copy_constructible_v<persisted_grouped_soa_coordinate_accepted_operation>);
static_assert(
    std::is_nothrow_move_constructible_v<persisted_grouped_soa_coordinate_accepted_operation>);

/// @brief runtime-sealed, move-only receipt for one grouped-SoA coordinate batch.  Each
/// logical job can be consumed exactly once and only with matching resident,
/// query-record and result identities.
class persisted_grouped_soa_coordinate_operation_receipt {
  public:
    persisted_grouped_soa_coordinate_operation_receipt() = default;
    persisted_grouped_soa_coordinate_operation_receipt(
        const persisted_grouped_soa_coordinate_operation_receipt&) = delete;
    persisted_grouped_soa_coordinate_operation_receipt&
    operator=(const persisted_grouped_soa_coordinate_operation_receipt&) = delete;
    persisted_grouped_soa_coordinate_operation_receipt(
        persisted_grouped_soa_coordinate_operation_receipt&& other) noexcept {
        move_from(other);
    }
    persisted_grouped_soa_coordinate_operation_receipt&
    operator=(persisted_grouped_soa_coordinate_operation_receipt&& other) noexcept {
        if (this != &other) {
            release_records();
            query_pipeline_token_ = {};
            grouped_soa_pipeline_token_ = {};
            move_from(other);
        }
        return *this;
    }

    [[nodiscard]] bool is_sealed() const noexcept {
        return sealed_ && operation_identity_ != 0u;
    }
    [[nodiscard]] std::size_t job_count() const noexcept {
        return is_sealed() ? records_.size() : 0u;
    }
    [[nodiscard]] std::size_t consumed_count() const noexcept {
        return is_sealed() ? accepted_count_ + burned_count_ : 0u;
    }
    [[nodiscard]] std::size_t accepted_count() const noexcept {
        return is_sealed() ? accepted_count_ : 0u;
    }
    [[nodiscard]] std::size_t burned_count() const noexcept {
        return is_sealed() ? burned_count_ : 0u;
    }
    [[nodiscard]] bool is_fully_consumed() const noexcept {
        return is_sealed() && !records_.empty() &&
               std::all_of(records_.begin(), records_.end(), [](const auto& record) {
                   return record.state != record_state::fresh;
               });
    }
    [[nodiscard]] bool are_all_jobs_accepted() const noexcept {
        return is_sealed() && is_fully_consumed() && accepted_count_ == records_.size() &&
               burned_count_ == 0u && has_valid_causal_tokens();
    }
    [[nodiscard]] runtime_gpu_execution_environment_snapshot environment_preview() const noexcept {
        return preview_environment();
    }
    [[nodiscard]] runtime_gpu_cache_metrics_audit cache_metrics_preview() const noexcept {
        return maximum_cache_metrics();
    }
    [[nodiscard]] persisted_grouped_soa_coordinate_operation_receipt_preview
    operation_receipt_preview() const noexcept {
        persisted_grouped_soa_coordinate_operation_receipt_preview result{};
        if (!are_all_jobs_accepted() || !query_pipeline_token_.is_valid_for_query_sketch() ||
            !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return result;
        }
        const runtime_pipeline_operation_audit& query = query_pipeline_token_.audit();
        const runtime_pipeline_operation_audit& shape = grouped_soa_pipeline_token_.audit();
        if (query.jobs.size() != 1u || query.jobs.front().committed_submissions != 2u ||
            shape.committed_submissions >
                (std::numeric_limits<std::uint64_t>::max)() - query.committed_submissions) {
            return result;
        }
        result.acceptance = persisted_grouped_soa_coordinate_operation_acceptance_audit{
            .operation_identity = operation_identity_,
            .expected_job_count = static_cast<std::uint64_t>(records_.size()),
            .consumed_count = static_cast<std::uint64_t>(consumed_count()),
            .accepted_count = static_cast<std::uint64_t>(accepted_count()),
            .burned_count = static_cast<std::uint64_t>(burned_count()),
            .query_pipeline_receipt_count = static_cast<std::uint64_t>(query.prepared_count),
            .shape_pipeline_receipt_count = static_cast<std::uint64_t>(shape.prepared_count),
            .committed_submission_count = query.committed_submissions + shape.committed_submissions,
            .seed_receipt_identity = query.seed_receipt_identity,
        };
        result.query_pipeline_operation_identity = query.operation_identity;
        result.shape_pipeline_operation_identity = shape.operation_identity;
        result.query_submission_ordinals[0] = query.jobs.front().submission_ordinals[0];
        result.query_submission_ordinals[1] = query.jobs.front().submission_ordinals[1];
        result.device_uuid = query.device_uuid;
        result.api_version = query.api_version;
        result.driver_version = query.driver_version;
        result.vendor_id = query.vendor_id;
        result.device_id = query.device_id;
        result.pipeline_cache_uuid = query.pipeline_cache_uuid;
        result.seed_sha256 = query.seed_sha256;
        result.seed_size = query.seed_size;
        result.manifest_checksum = query.manifest_checksum;
        if (query.cache_qualification != shape.cache_qualification) {
            return {};
        }
        result.cache_qualification = query.cache_qualification;
        result.maximum_cache_metrics = maximum_cache_metrics();
        return result.is_internally_consistent()
                   ? result
                   : persisted_grouped_soa_coordinate_operation_receipt_preview{};
    }
    [[nodiscard]] std::optional<persisted_grouped_soa_coordinate_job_receipt_preview>
    job_evidence_preview(const std::size_t problem_index) const noexcept {
        if (!are_all_jobs_accepted() || problem_index >= records_.size() ||
            !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return std::nullopt;
        }
        const runtime_pipeline_operation_audit& shape = grouped_soa_pipeline_token_.audit();
        if (problem_index >= shape.jobs.size())
            return std::nullopt;
        const runtime_pipeline_job_use_audit& use = shape.jobs[problem_index];
        if (use.committed_submissions != 4u ||
            use.submission_operation_identity != shape.operation_identity) {
            return std::nullopt;
        }
        persisted_grouped_soa_coordinate_job_receipt_preview result{
            .audit = records_[problem_index].audit,
            .shape_pipeline_operation_identity = use.submission_operation_identity,
            .shape_submission_ordinals = use.submission_ordinals,
        };
        return result.is_internally_consistent()
                   ? std::optional<persisted_grouped_soa_coordinate_job_receipt_preview>{result}
                   : std::nullopt;
    }

    /// @brief Contract-only test hook. It returns no receipt and therefore
    /// grants no runtime authority. It only lets the portable suite exercise
    /// private sealing, move invalidation, tamper burning, and one-shot consume.
    /// @return `true` when every one-shot receipt check succeeds.
    [[nodiscard]] static bool self_test_one_shot_contract(
        const std::array<persisted_grouped_soa_coordinate_job_audit, 2>& audits,
        const std::array<persisted_grouped_soa_coordinate_job_expectation, 2>& expectations) {
        if (audits[0].operation_identity == 0u ||
            audits[0].operation_identity != audits[1].operation_identity ||
            audits[0].problem_index != 0u || audits[1].problem_index != 1u ||
            !audits[0].is_internally_consistent() || !audits[1].is_internally_consistent()) {
            return false;
        }
        persisted_grouped_soa_coordinate_operation_receipt issued{};
        issued.operation_identity_ = audits[0].operation_identity;
        try {
            issued.admit_records(2u);
        } catch (...) {
            return false;
        }
        issued.records_[0].audit = audits[0];
        issued.records_[1].audit = audits[1];
        issued.test_causal_accepted_ = true;
        issued.sealed_ = true;

        persisted_grouped_soa_coordinate_operation_receipt receipt{std::move(issued)};
        if (issued.is_sealed() || issued.job_count() != 0u || issued.records_capacity_permit_ ||
            !receipt.records_capacity_permit_ || !receipt.is_sealed() ||
            receipt.job_count() != 2u) {
            return false;
        }
        auto first = receipt.consume(expectations[0]);
        if (!first.has_value() || !first->is_valid())
            return false;
        if (receipt.is_fully_consumed())
            return false;
        persisted_grouped_soa_coordinate_job_receipt moved_first{std::move(*first)};
        if (first->is_valid() || !moved_first.is_valid())
            return false;
        if (receipt.consume(expectations[0]).has_value())
            return false;

        auto tampered_second = expectations[1];
        tampered_second.source_identity ^= 1u;
        if (receipt.consume(tampered_second).has_value())
            return false;
        if (receipt.consume(expectations[1]).has_value())
            return false;
        if (!receipt.is_fully_consumed() || receipt.accepted_count() != 1u ||
            receipt.burned_count() != 1u || receipt.are_all_jobs_accepted())
            return false;

        persisted_grouped_soa_coordinate_operation_receipt all_valid{};
        all_valid.operation_identity_ = audits[0].operation_identity;
        try {
            all_valid.admit_records(2u);
        } catch (...) {
            return false;
        }
        all_valid.records_[0].audit = audits[0];
        all_valid.records_[1].audit = audits[1];
        all_valid.test_causal_accepted_ = true;
        all_valid.sealed_ = true;
        auto valid_first = all_valid.consume(expectations[0]);
        auto valid_second = all_valid.consume(expectations[1]);
        return valid_first.has_value() && valid_first->is_valid() && valid_second.has_value() &&
               valid_second->is_valid() && !all_valid.consume(expectations[0]).has_value() &&
               !all_valid.consume(expectations[1]).has_value() && all_valid.is_fully_consumed() &&
               all_valid.accepted_count() == 2u && all_valid.burned_count() == 0u &&
               all_valid.records_capacity_permit_ && all_valid.are_all_jobs_accepted();
    }

    [[nodiscard]] std::optional<persisted_grouped_soa_coordinate_job_receipt>
    consume(const persisted_grouped_soa_coordinate_job_expectation& expected) noexcept {
        if (!is_sealed() || expected.problem_index >= records_.size()) {
            return std::nullopt;
        }
        record& record = records_[expected.problem_index];
        if (record.state != record_state::fresh)
            return std::nullopt;
        const auto& audit = record.audit;
        const auto& query = expected.query_record;
        const bool identities_match =
            audit.problem_index == expected.problem_index &&
            audit.operation_identity == operation_identity_ &&
            audit.query_authority_identity == expected.query_authority_identity &&
            audit.resident_descriptors == expected.resident_descriptors &&
            audit.resident_controls == expected.resident_controls &&
            audit.resident_positions == expected.resident_positions &&
            audit.query_minimizer_buffer == expected.query_minimizer_buffer &&
            audit.device_uuid == expected.device_uuid &&
            audit.descriptor_bytes == expected.descriptor_bytes &&
            audit.control_bytes == expected.control_bytes &&
            audit.position_bytes == expected.position_bytes &&
            audit.position_count == expected.position_count &&
            audit.max_occ_cap == expected.max_occ_cap &&
            audit.partition_count == expected.partition_count &&
            audit.abi_revision == expected.abi_revision &&
            audit.generation_id == expected.generation_id &&
            audit.source_identity == expected.source_identity &&
            audit.cold_load_identity == expected.cold_load_identity &&
            audit.target_len == expected.target_len && audit.query_index == expected.query_index &&
            audit.logical_target_row == expected.logical_target_row &&
            audit.logical_query_ordinal == expected.logical_query_ordinal &&
            audit.query_record.minimizer_offset == query.minimizer_offset &&
            audit.query_record.minimizer_count == query.minimizer_count &&
            audit.query_record.query_len == query.query_len &&
            audit.query_record.flags == query.flags &&
            are_coordinate_result_wires_equal(audit.result, expected.result) &&
            audit.configuration == expected.configuration;
        if (!identities_match || !audit.is_internally_consistent() || !has_valid_causal_tokens()) {
            record.state = record_state::burned;
            ++burned_count_;
            return std::nullopt;
        }
        record.state = record_state::accepted;
        ++accepted_count_;
        persisted_grouped_soa_coordinate_job_receipt job_receipt{};
        job_receipt.audit_ = audit;
        job_receipt.pipeline_operation_identity_ = operation_identity_;
        job_receipt.issued_ = true;
        return job_receipt;
    }

  private:
    friend class runtime;

    enum class record_state : std::uint8_t {
        fresh,
        accepted,
        burned,
    };

    struct record {
        persisted_grouped_soa_coordinate_job_audit audit{};
        record_state state = record_state::fresh;
    };

    struct pipeline_receipt_union {
        std::array<shader_variant_key, k_runtime_pipeline_prepared_slots> keys{};
        std::array<const runtime_metal_archive_pipeline_receipt*, k_runtime_pipeline_prepared_slots>
            receipts{};
        std::size_t count = 0u;
        bool valid = false;
    };

    [[nodiscard]] pipeline_receipt_union receipt_union() const noexcept {
        pipeline_receipt_union result{};
        if (!sealed_ || test_causal_accepted_ ||
            !query_pipeline_token_.is_valid_for_query_sketch() ||
            !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return result;
        }
        const auto merge = [&result](const runtime_pipeline_operation_token& operation) {
            for (std::size_t i = 0u; i < operation.audit_.prepared_count; ++i) {
                std::size_t slot = result.count;
                for (std::size_t existing = 0u; existing < result.count; ++existing) {
                    if (result.keys[existing] == operation.audit_.prepared[i].shader_key) {
                        slot = existing;
                        break;
                    }
                }
                if (slot == result.count) {
                    if (result.count == result.keys.size())
                        return false;
                    result.keys[result.count] = operation.audit_.prepared[i].shader_key;
                    result.receipts[result.count] = operation.receipts_[i];
                    ++result.count;
                } else if (result.receipts[slot] != operation.receipts_[i]) {
                    return false;
                }
            }
            return true;
        };
        if (!merge(query_pipeline_token_) || !merge(grouped_soa_pipeline_token_) ||
            result.count == 0u ||
            result.count != grouped_soa_pipeline_token_.audit_.prepared_count + 1u) {
            return {};
        }
        result.valid = true;
        return result;
    }

    [[nodiscard]] runtime_gpu_execution_environment_snapshot preview_environment() const noexcept {
        const pipeline_receipt_union union_receipts = receipt_union();
        if (!union_receipts.valid)
            return {};
        const runtime_pipeline_operation_audit& query = query_pipeline_token_.audit_;
        const runtime_pipeline_operation_audit& shape = grouped_soa_pipeline_token_.audit_;
        if (query.device_uuid != shape.device_uuid || query.api_version != shape.api_version ||
            query.driver_version != shape.driver_version || query.vendor_id != shape.vendor_id ||
            query.device_id != shape.device_id ||
            query.pipeline_cache_uuid != shape.pipeline_cache_uuid ||
            query.seed_sha256 != shape.seed_sha256 || query.seed_size != shape.seed_size ||
            query.manifest_checksum != shape.manifest_checksum ||
            query.seed_receipt_identity != shape.seed_receipt_identity ||
            query.archive_set != shape.archive_set ||
            query.cache_qualification != shape.cache_qualification) {
            return {};
        }
        vulkan_feature_set identity{};
        identity.api_version = query.api_version;
        identity.driver_version = query.driver_version;
        identity.vendor_id = query.vendor_id;
        identity.device_id = query.device_id;
        identity.pipeline_cache_uuid = query.pipeline_cache_uuid;
        runtime_gpu_execution_environment_snapshot result{
            .gpu_environment_present = true,
            .gpu_device_uuid = query.device_uuid,
            .gpu_driver_identity = canonical_vulkan_driver_identity(identity),
            .cache_policy = runtime_pipeline_cache_policy::immutable_preseed_all_hit,
            .seed_sha256 = query.seed_sha256,
            .seed_size = query.seed_size,
            .manifest_checksum = query.manifest_checksum,
            .pipeline_cache_all_hit = true,
        };
        return result.is_internally_consistent() ? result
                                                 : runtime_gpu_execution_environment_snapshot{};
    }

    [[nodiscard]] runtime_gpu_cache_metrics_audit maximum_cache_metrics() const noexcept {
        const pipeline_receipt_union union_receipts = receipt_union();
        if (!union_receipts.valid)
            return {};
        runtime_gpu_cache_metrics_audit result{
            .token_identity = operation_identity_,
            .gpu_pipeline_cache_loads = 1u,
            .gpu_pipeline_cache_seed_bytes = query_pipeline_token_.audit_.seed_size,
        };
        const runtime_metal_archive_pipeline_receipt* first = union_receipts.receipts[0];
        if (first == nullptr || !first->is_valid())
            return {};
        result.gpu_pipeline_cache_load_ns = first->audit().seed_load_ns;
        for (std::size_t i = 0u; i < union_receipts.count; ++i) {
            const runtime_metal_archive_pipeline_receipt* receipt = union_receipts.receipts[i];
            if (receipt == nullptr || !receipt->is_valid() ||
                receipt->audit().seed_load_ns != result.gpu_pipeline_cache_load_ns ||
                result.gpu_pipeline_creates == (std::numeric_limits<std::uint64_t>::max)() ||
                receipt->audit().pipeline_create_ns >
                    (std::numeric_limits<std::uint64_t>::max)() - result.gpu_pipeline_creation_ns) {
                return {};
            }
            ++result.gpu_pipeline_creates;
            result.gpu_pipeline_creation_ns += receipt->audit().pipeline_create_ns;
        }
        return result.is_internally_consistent() ? result : runtime_gpu_cache_metrics_audit{};
    }

    void admit_records(std::size_t record_count) {
        using bioseq::common::saturating_mul;
        if (record_count == 0u || !records_.empty() || records_capacity_permit_ ||
            record_count > records_.max_size()) {
            throw vulkan_runtime_error("persisted grouped-SoA operation record shape is invalid");
        }
        const std::uint64_t logical =
            saturating_mul(static_cast<std::uint64_t>(record_count), sizeof(record));
        const std::uint64_t peak =
            saturating_mul(logical, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (logical == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw vulkan_runtime_error(
                "persisted grouped-SoA operation record admission saturated");
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            peak, &failure);
        if (!permit) {
            throw vulkan_runtime_error(shader_cache_message(
                "persisted grouped-SoA operation record admission denied: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                " requested=",
                failure.requested_bytes));
        }
        try {
            records_.resize(record_count);
        } catch (const std::bad_alloc&) {
            std::vector<record> empty;
            records_.swap(empty);
            throw vulkan_runtime_error(
                "persisted grouped-SoA operation record allocation failed after admission");
        }
        const std::uint64_t retained =
            saturating_mul(static_cast<std::uint64_t>(records_.capacity()), sizeof(record));
        if (retained == (std::numeric_limits<std::uint64_t>::max)() || retained > permit->bytes()) {
            std::vector<record> empty;
            records_.swap(empty);
            throw vulkan_runtime_error(
                "persisted grouped-SoA operation records exceeded admitted capacity");
        }
        permit->shrink_to(retained);
        records_capacity_permit_ = std::move(*permit);
    }

    void release_records() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            records_, records_capacity_permit_);
    }

    [[nodiscard]] bool has_valid_causal_tokens() const noexcept {
        return test_causal_accepted_ ||
               (query_pipeline_token_.is_valid_for_query_sketch() &&
                grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate());
    }

    void move_from(persisted_grouped_soa_coordinate_operation_receipt& other) noexcept {
        operation_identity_ = other.operation_identity_;
        records_ = std::move(other.records_);
        records_capacity_permit_ = std::move(other.records_capacity_permit_);
        query_pipeline_token_ = std::move(other.query_pipeline_token_);
        grouped_soa_pipeline_token_ = std::move(other.grouped_soa_pipeline_token_);
        accepted_count_ = other.accepted_count_;
        burned_count_ = other.burned_count_;
        test_causal_accepted_ = other.test_causal_accepted_;
        sealed_ = other.sealed_;
        other.operation_identity_ = 0u;
        other.records_.clear();
        other.accepted_count_ = 0u;
        other.burned_count_ = 0u;
        other.test_causal_accepted_ = false;
        other.sealed_ = false;
    }

    std::uint64_t operation_identity_ = 0u;
    // Declared before records_: record capacity is destroyed before release.
    bioseq::parallelism::memory_permit records_capacity_permit_{};
    std::vector<record> records_{};
    runtime_pipeline_operation_token query_pipeline_token_{};
    runtime_pipeline_operation_token grouped_soa_pipeline_token_{};
    std::size_t accepted_count_ = 0u;
    std::size_t burned_count_ = 0u;
    bool test_causal_accepted_ = false;
    bool sealed_ = false;
};

static_assert(!std::is_copy_constructible_v<persisted_grouped_soa_coordinate_operation_receipt>);
static_assert(
    std::is_nothrow_move_constructible_v<persisted_grouped_soa_coordinate_operation_receipt>);
static_assert(!persisted_grouped_soa_coordinate_job_audit{}.is_internally_consistent());

namespace runtime_detail {

/// @brief Fixed final-publication payload prepared before the immutable live gate.
/// Dropping this object without committing its claim plan has no side effects.
struct persisted_grouped_soa_coordinate_finalization_plan {
    runtime_gpu_cache_metrics_claim_plan cache_claim_plan{};
    runtime_gpu_execution_environment_snapshot environment{};
    persisted_grouped_soa_coordinate_operation_acceptance_audit acceptance{};
    bool valid = false;

    [[nodiscard]] bool is_valid() const noexcept {
        return valid && cache_claim_plan.is_valid() && environment.is_internally_consistent() &&
               acceptance.is_internally_consistent();
    }
};

static_assert(!std::is_copy_constructible_v<persisted_grouped_soa_coordinate_finalization_plan>);
static_assert(
    std::is_nothrow_move_constructible_v<persisted_grouped_soa_coordinate_finalization_plan>);

} // namespace runtime_detail

/// @brief Coordinate results and causal receipt produced by a batched GPU run.
struct gpu_seed_coordinate_batch_result {
    std::vector<gpu_coordinate_result_wire> results{};
    gpu_seed_coordinate_batch_stats stats{};
    persisted_grouped_soa_coordinate_operation_receipt operation_receipt{};
    /// @brief Dispatch and identity statistics for the direct grouped-SoA route.
    struct grouped_soa_direct_route_stats {
        bool selected = false;
        bool direct_count_scan_emit_completed = false;
        std::uint64_t problem_count = 0;
        std::uint64_t generation_id = 0;
        std::uint64_t source_identity = 0;
        std::uint64_t cold_load_identity = 0;
        std::uint64_t count_dispatches = 0;
        std::uint64_t emit_dispatches = 0;
        std::uint64_t count_status_checks = 0;
        std::uint64_t emit_status_checks = 0;
        std::uint32_t status_or = 0;
        std::uint64_t cpu_or_simd_fallbacks = 0;
    } grouped_soa_direct{};
};

} // namespace bioseq::gpu::vulkan

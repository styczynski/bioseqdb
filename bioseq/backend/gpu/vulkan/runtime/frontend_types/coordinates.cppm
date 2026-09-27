/**
 * @file coordinates.cppm
 * @brief Defines seed-to-coordinate inputs, audits, receipts, and results.
 */
module;

#include <cstddef>
#include <cstdint>
#include <new>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:frontend_types.coordinates;

import :receipts;
import :frontend_types.sketch;
import :frontend_types.arena;
import :frontend_types.indexing;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
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
    invalid = 0,                    ///< Missing or mixed resident inputs.
    source_direct_sorted_dense = 1, ///< Sorted dense slot and position buffers.
    grouped_soa = 2,                ///< Authenticated persisted grouped-SoA buffers.
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

/// @brief Determines which mutually exclusive resident-index representation a job uses.
/// @param[in] job Resident buffers and representation fields to classify.
/// @return The selected representation, or `invalid` for mixed or incomplete inputs.
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
    vulkan_tiled_dp = 0, ///< Exact tiled dynamic-programming chaining route.
    coordinate_vote = 1, ///< Approximate diagonal-vote prototype route.
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
    gpu_persisted_mapq_config mapq{};

    /// @brief Validates the complete dispatch-affecting configuration identity.
    /// @return `true` when all required tiled-DP fields are supported and nonzero.
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return kmer_size != 0u && kmer_size <= 255u && max_occ >= 0 &&
               coordinate_algorithm == gpu_seed_coordinate_algorithm::vulkan_tiled_dp &&
               chain_key.tile_size != 0u && chain_key.local_size != 0u && probe_limit != 0u &&
               (mapq == gpu_persisted_mapq_config{} || mapq.is_valid());
    }
    /// @brief Compares every dispatch-affecting configuration field exactly.
    [[nodiscard]] bool
    operator==(const persisted_grouped_soa_coordinate_config_identity&) const = default;
};

/// @brief Builds an exact configuration identity, preserving float bit patterns.
/// @param[in] kmer_size K-mer span used during seeding.
/// @param[in] max_occ Maximum target occurrences emitted per minimizer.
/// @param[in] coordinate_algorithm Coordinate reduction algorithm.
/// @param[in] chain_key Chaining pipeline specialization.
/// @param[in] max_dist_x Maximum reference-coordinate chaining distance.
/// @param[in] max_dist_y Maximum query-coordinate chaining distance.
/// @param[in] bandwidth Chaining diagonal bandwidth.
/// @param[in] max_iter Maximum predecessors examined per anchor.
/// @param[in] gap_penalty Chaining gap penalty preserved by bit pattern.
/// @param[in] skip_penalty Chaining skip penalty preserved by bit pattern.
/// @param[in] max_chain_skip Maximum consecutive unproductive predecessors.
/// @param[in] min_chain_score Minimum accepted chain score.
/// @param[in] min_seed_count Minimum accepted seed count.
/// @param[in] probe_limit Maximum coordinate candidates examined.
/// @return Fully populated configuration identity.
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
    std::uint32_t probe_limit,
    gpu_persisted_mapq_config mapq = {}) noexcept {
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
        .mapq = mapq,
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
    std::uint64_t chain_candidate_dispatches = 0;
    std::uint64_t coordinate_vote_dispatches = 0;

    /// @brief Compares every completed batch counter.
    [[nodiscard]] constexpr bool
    operator==(const gpu_seed_coordinate_batch_stats&) const noexcept = default;
};

/// @brief Compares every coordinate-result wire field.
/// @param[in] lhs First result wire.
/// @param[in] rhs Second result wire.
/// @return `true` when the two wire values are bitwise-field equivalent.
[[nodiscard]] inline constexpr bool
are_coordinate_result_wires_equal(const gpu_coordinate_result_wire& lhs,
                                  const gpu_coordinate_result_wire& rhs) noexcept {
    return lhs.query_start == rhs.query_start && lhs.query_end == rhs.query_end &&
           lhs.target_start == rhs.target_start && lhs.target_end == rhs.target_end &&
           lhs.match_length == rhs.match_length && lhs.block_length == rhs.block_length &&
           lhs.chain_score == rhs.chain_score && lhs.seed_count == rhs.seed_count &&
           lhs.is_reverse == rhs.is_reverse && lhs.is_valid == rhs.is_valid;
}

/// @brief Compares every candidate-header field without relying on padding bytes.
[[nodiscard]] inline constexpr bool
are_chain_candidate_headers_equal(const gpu_chain_candidate_header_wire& lhs,
                                  const gpu_chain_candidate_header_wire& rhs) noexcept {
    return lhs.candidate_offset == rhs.candidate_offset &&
           lhs.candidate_count == rhs.candidate_count && lhs.retained_count == rhs.retained_count &&
           lhs.primary_count == rhs.primary_count && lhs.alignment_offset == rhs.alignment_offset &&
           lhs.alignment_count == rhs.alignment_count &&
           lhs.repetitive_span == rhs.repetitive_span && lhs.status == rhs.status &&
           lhs.candidate_capacity == rhs.candidate_capacity && lhs.reserved == rhs.reserved;
}

/// @brief Validates a coordinate result against query and target bounds.
/// @param[in] result Wire value to validate.
/// @param[in] query_len Query length in symbols.
/// @param[in] target_len Target length in symbols.
/// @return `true` for the canonical empty result or a bounded valid alignment.
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

/// @brief Validates one accepted-chain wire without trusting device offsets.
[[nodiscard]] inline constexpr bool
is_chain_candidate_wire_well_formed(const gpu_chain_candidate_wire& candidate,
                                    const std::uint32_t problem_index,
                                    const std::uint32_t candidate_index,
                                    const std::uint64_t query_len,
                                    const std::uint64_t target_len) noexcept {
    const std::uint32_t required_flags = k_gpu_chain_candidate_flag_valid;
    return candidate.problem_index == problem_index &&
           candidate.candidate_index == candidate_index &&
           (candidate.flags & required_flags) == required_flags &&
           (candidate.flags & ~k_gpu_chain_candidate_flags) == 0u &&
           (candidate.flags & k_gpu_chain_candidate_flag_aligned) == 0u &&
           candidate.query_start < candidate.query_end && candidate.query_end <= query_len &&
           candidate.target_start < candidate.target_end && candidate.target_end <= target_len &&
           candidate.match_length != 0u && candidate.match_length <= candidate.block_length &&
           candidate.block_length <= (candidate.query_end - candidate.query_start) +
                                         (candidate.target_end - candidate.target_start) &&
           candidate.chain_score > 0 && candidate.original_chain_score == candidate.chain_score &&
           candidate.seed_count > 0 && candidate.parent_index >= 0 &&
           static_cast<std::uint32_t>(candidate.parent_index) <= candidate_index &&
           candidate.secondary_chain_score >= 0 && candidate.secondary_count >= 0 &&
           candidate.dp_score == 0 && candidate.dp_max_score == 0 &&
           candidate.secondary_dp_max == 0;
}

/// @brief Hashes explicit candidate fields, excluding host padding and addresses.
[[nodiscard]] inline std::uint64_t
chain_candidate_digest(const gpu_chain_candidate_header_wire& header,
                       const std::span<const gpu_chain_candidate_wire> candidates) noexcept {
    std::uint64_t digest = bioseq::common::k_fnv1a_64_standard_offset_basis;
    const auto mix = [&digest](const auto& value) noexcept {
        digest = bioseq::common::extend_fnv1a_64(
            digest, std::as_bytes(std::span{std::addressof(value), std::size_t{1}}));
    };
    mix(header.candidate_offset);
    mix(header.candidate_count);
    mix(header.retained_count);
    mix(header.primary_count);
    mix(header.alignment_offset);
    mix(header.alignment_count);
    mix(header.repetitive_span);
    mix(header.status);
    mix(header.candidate_capacity);
    mix(header.reserved);
    for (const gpu_chain_candidate_wire& candidate : candidates) {
        mix(candidate.query_start);
        mix(candidate.query_end);
        mix(candidate.target_start);
        mix(candidate.target_end);
        mix(candidate.match_length);
        mix(candidate.block_length);
        mix(candidate.chain_score);
        mix(candidate.original_chain_score);
        mix(candidate.seed_count);
        mix(candidate.parent_index);
        mix(candidate.secondary_chain_score);
        mix(candidate.secondary_count);
        mix(candidate.dp_score);
        mix(candidate.dp_max_score);
        mix(candidate.secondary_dp_max);
        mix(candidate.candidate_index);
        mix(candidate.problem_index);
        mix(candidate.flags);
    }
    digest &= (std::uint64_t{1} << 63u) - 1u;
    return digest == 0u ? bioseq::common::k_fnv1a_64_alternate_offset_basis : digest;
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
    gpu_chain_candidate_header_wire candidate_header{};
    std::uint64_t candidate_digest = 0u;
    persisted_grouped_soa_coordinate_config_identity configuration{};
};

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/// @brief Word ranges in the failure-only grouped-SoA count shader sentinel.
enum class benchmark_e2e_grouped_soa_count_trace_word : std::size_t {
    request_magic = 0u,
    guard_magic = 1u,
    completion_magic = 2u,
    requested_partition_ordinal = 3u,
    push_begin = 4u,
    query_record_begin = 31u,
    query_words_begin = 35u,
    query_hash_begin = 39u,
    partition_descriptor_begin = 42u,
    lookup_begin = 68u,
    decision_begin = 90u,
    word_count = 97u,
};

inline constexpr std::uint32_t k_benchmark_e2e_grouped_soa_count_trace_request_magic = 0x42535452u;
inline constexpr std::uint32_t k_benchmark_e2e_grouped_soa_count_trace_guard_magic = 0x47534f41u;
inline constexpr std::uint32_t k_benchmark_e2e_grouped_soa_count_trace_completion_magic =
    0x4253444eu;

static_assert(
    static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::push_begin) + 27u ==
    static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::query_record_begin));
static_assert(
    static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::query_record_begin) + 4u ==
    static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::query_words_begin));
static_assert(
    static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::query_words_begin) + 4u ==
    static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::query_hash_begin));
static_assert(
    static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::query_hash_begin) + 3u ==
    static_cast<std::size_t>(
        benchmark_e2e_grouped_soa_count_trace_word::partition_descriptor_begin));
static_assert(static_cast<std::size_t>(
                  benchmark_e2e_grouped_soa_count_trace_word::partition_descriptor_begin) +
                  26u ==
              static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::lookup_begin));
static_assert(static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::lookup_begin) +
                  22u ==
              static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::decision_begin));
static_assert(static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::decision_begin) +
                  7u ==
              static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::word_count));
#endif

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
    gpu_chain_candidate_header_wire candidate_header{};
    std::uint64_t candidate_digest = 0u;
    persisted_grouped_soa_coordinate_config_identity configuration{};
    std::uint32_t query_tile_count = 0u;
    std::uint64_t query_count_dispatches = 0;
    std::uint64_t query_scan_dispatches = 0;
    std::uint64_t query_emit_dispatches = 0;
    std::uint64_t query_completed_roundtrips = 0;
    std::uint64_t binding_window_count = 0; ///< Complete bounded partition windows required by this job.
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
    std::uint64_t chain_candidate_dispatches = 0;
    std::uint64_t count_submit_wait_identity = 0;
    std::uint64_t emit_submit_wait_identity = 0;
    std::uint64_t sort_range_submit_wait_identity = 0;
    std::uint64_t chain_coordinate_submit_wait_identity = 0;
    std::uint64_t count_status_checks = 0;
    std::uint64_t emit_status_checks = 0;
    std::uint64_t result_readbacks = 0;
    std::uint64_t candidate_readbacks = 0;
    std::uint32_t status_or = 0;
    std::uint64_t coordinate_vote_dispatches = 0;
    std::uint64_t cpu_or_simd_biological_calls = 0;
    std::uint64_t recompute_calls = 0;
    std::uint64_t fallback_calls = 0;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    // Failure-only readback from the completed grouped-SoA count/scan submission.
    // Production builds do not carry these fields or their capture cost.
    bool count_scan_failure_probe_available = false;
    std::uint32_t count_scan_probe_first_count = 0u;
    std::uint32_t count_scan_probe_first_prefix = 0u;
    std::uint32_t count_scan_probe_boundary_index = 0u;
    std::uint32_t count_scan_probe_boundary_count = 0u;
    std::uint32_t count_scan_probe_boundary_prefix = 0u;
    std::uint32_t count_scan_probe_last_index = 0u;
    std::uint32_t count_scan_probe_last_count = 0u;
    std::uint32_t count_scan_probe_last_prefix = 0u;
    std::uint32_t count_scan_probe_block_count = 0u;
    std::uint32_t count_scan_probe_first_block_offset = 0u;
    std::uint32_t count_scan_probe_last_block_offset = 0u;
    bool device_count_trace_attempted = false;
    bool device_count_trace_available = false;
    std::array<std::uint32_t,
               static_cast<std::size_t>(benchmark_e2e_grouped_soa_count_trace_word::word_count)>
        device_count_trace_words{};
#endif

    /// @brief Validates identities, dispatch causality, and result shape for one job.
    /// @return `true` when the audit proves a complete supported GPU path.
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
        const bool direct_front_end = binding_window_count != 0u && binding_window_count <= partition_count &&
                                      direct_count_dispatches == binding_window_count &&
                                      direct_scan_dispatches == 3u &&
                                      count_submit_wait_identity != 0u && count_status_checks == 1u;
        if (!direct_front_end)
            return false;
        if (anchor_count == 0u) {
            return direct_emit_dispatches == 0u && sort_operations == 0u &&
                   range_build_dispatches == 0u && range_count == 0u && chain_dispatches == 0u &&
                   chain_backtrack_dispatches == 0u && coordinate_dispatches == 0u &&
                   chain_candidate_dispatches == 0u && candidate_readbacks == 0u &&
                   candidate_digest == 0u &&
                   are_chain_candidate_headers_equal(candidate_header, {}) &&
                   emit_submit_wait_identity == 0u && sort_range_submit_wait_identity == 0u &&
                   chain_coordinate_submit_wait_identity == 0u && emit_status_checks == 0u &&
                   result_readbacks == 0u &&
                   are_coordinate_result_wires_equal(result, gpu_coordinate_result_wire{});
        }
        const bool emitted_and_ranged =
            direct_emit_dispatches == binding_window_count && sort_operations == 1u && range_build_dispatches == 1u &&
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
                   chain_candidate_dispatches == 0u && candidate_readbacks == 0u &&
                   candidate_digest == 0u &&
                   are_chain_candidate_headers_equal(candidate_header, {}) &&
                   result_readbacks == 0u &&
                   are_coordinate_result_wires_equal(result, gpu_coordinate_result_wire{});
        }
        const bool mapq_selected = configuration.mapq.is_valid();
        const bool candidate_shape =
            candidate_header.status == k_gpu_mapq_status_ok && candidate_header.reserved == 0u &&
            candidate_header.candidate_capacity == configuration.mapq.candidate_capacity &&
            candidate_header.candidate_count <= candidate_header.candidate_capacity &&
            candidate_header.retained_count <= candidate_header.candidate_count &&
            candidate_header.primary_count <= candidate_header.retained_count &&
            candidate_header.alignment_offset == 0u && candidate_header.alignment_count == 0u &&
            candidate_digest != 0u && candidate_readbacks == 1u;
        const bool reducer_shape =
            mapq_selected ? chain_candidate_dispatches == 1u && candidate_shape
                          : chain_candidate_dispatches == 0u && candidate_readbacks == 0u &&
                                candidate_digest == 0u &&
                                are_chain_candidate_headers_equal(candidate_header, {});
        return chain_dispatches == 1u && chain_backtrack_dispatches == 1u && reducer_shape &&
               coordinate_dispatches == 1u && chain_coordinate_submit_wait_identity != 0u &&
               chain_coordinate_submit_wait_identity != sort_range_submit_wait_identity &&
               chain_coordinate_submit_wait_identity != emit_submit_wait_identity &&
               chain_coordinate_submit_wait_identity != count_submit_wait_identity &&
               result_readbacks == 1u &&
               is_coordinate_result_wire_well_formed(result, query_record.query_len, target_len) &&
               (!mapq_selected ||
                ((candidate_header.candidate_count == 0u) == (result.is_valid == 0u)));
    }
};

/// @brief Unforgeable consumed view of one accepted audit record.
class persisted_grouped_soa_coordinate_job_receipt {
  public:
    /// @brief Constructs an empty invalid job receipt.
    persisted_grouped_soa_coordinate_job_receipt() = default;
    /// @brief Prevents copying one-shot receipt authority.
    persisted_grouped_soa_coordinate_job_receipt(
        const persisted_grouped_soa_coordinate_job_receipt&) = delete;
    /// @brief Prevents copy-assignment of one-shot receipt authority.
    persisted_grouped_soa_coordinate_job_receipt&
    operator=(const persisted_grouped_soa_coordinate_job_receipt&) = delete;
    /// @brief Transfers receipt authority and invalidates the source.
    persisted_grouped_soa_coordinate_job_receipt(
        persisted_grouped_soa_coordinate_job_receipt&& other) noexcept {
        move_from(other);
    }
    /// @brief Replaces this receipt with transferred authority.
    /// @return This receipt.
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

    /// @brief Validates issuance, operation identity, and the sealed audit.
    /// @return `true` when this object carries consumable job evidence.
    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && pipeline_operation_identity_ != 0u && audit_.is_internally_consistent();
    }
    /// @brief Returns the immutable job audit.
    /// @return Audit bound to this receipt.
    [[nodiscard]] const persisted_grouped_soa_coordinate_job_audit& audit() const noexcept {
        return audit_;
    }

  private:
    friend class persisted_grouped_soa_coordinate_operation_receipt;

    /// @brief Transfers receipt fields and clears the source.
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
    std::uint64_t alignment_pipeline_receipt_count = 0u;
    std::uint64_t alignment_submission_count = 0u;
    std::uint64_t mapq_pipeline_receipt_count = 0u;
    std::uint64_t mapq_submission_count = 0u;
    std::uint64_t mapq_result_count = 0u;
    std::uint64_t mapq_published_count = 0u;
    std::uint64_t mapq_result_digest = 0u;
    std::uint64_t committed_submission_count = 0u;
    std::uint64_t seed_receipt_identity = 0u;

    /// @brief Validates aggregate one-shot acceptance and receipt accounting.
    /// @return `true` when every expected job was accepted without burning.
    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        if (alignment_submission_count >
            (std::numeric_limits<std::uint64_t>::max)() - mapq_submission_count) {
            return false;
        }
        const std::uint64_t downstream_submissions =
            alignment_submission_count + mapq_submission_count;
        if (downstream_submissions > (std::numeric_limits<std::uint64_t>::max)() - 3u) {
            return false;
        }
        return operation_identity != 0u && expected_job_count != 0u &&
               consumed_count == expected_job_count && accepted_count == expected_job_count &&
               burned_count == 0u && query_pipeline_receipt_count == 3u &&
               shape_pipeline_receipt_count >= 7u && shape_pipeline_receipt_count <= 10u &&
               alignment_pipeline_receipt_count <= 1u &&
               ((alignment_pipeline_receipt_count == 0u) == (alignment_submission_count == 0u)) &&
               mapq_pipeline_receipt_count <= 1u &&
               ((mapq_pipeline_receipt_count == 0u) == (mapq_submission_count == 0u)) &&
               mapq_submission_count == mapq_pipeline_receipt_count &&
               ((mapq_pipeline_receipt_count == 0u) == (mapq_result_digest == 0u)) &&
               mapq_published_count <= mapq_result_count &&
               (mapq_pipeline_receipt_count != 0u ||
                (mapq_result_count == 0u && mapq_published_count == 0u)) &&
               committed_submission_count >= 3u + downstream_submissions &&
               seed_receipt_identity != 0u;
    }
    /// @brief Compares all aggregate acceptance counters and identities.
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

    /// @brief Validates operation identities, environment binding, and cache metrics.
    /// @return `true` when the preview is complete and causally consistent.
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
    /// @brief Compares every operation-preview field.
    [[nodiscard]] constexpr bool operator==(
        const persisted_grouped_soa_coordinate_operation_receipt_preview&) const noexcept = default;
};

/// @brief Exact full-stage job payload plus its structured Shape submission receipts.
struct persisted_grouped_soa_coordinate_job_receipt_preview {
    persisted_grouped_soa_coordinate_job_audit audit{};
    std::uint64_t shape_pipeline_operation_identity = 0u;
    std::array<std::uint64_t, 4> shape_submission_ordinals{};

    /// @brief Validates full-stage job evidence and unique submission ordinals.
    /// @return `true` when the job proves the complete shape pipeline.
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

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/// @brief Authority-free copy of existing per-job receipt fields for E2E failures.
struct persisted_grouped_soa_coordinate_job_failure_diagnostic {
    persisted_grouped_soa_coordinate_job_audit audit{};
    bool operation_sealed = false;
    bool all_jobs_accepted = false;
    std::size_t operation_job_count = 0u;
    bool shape_pipeline_token_valid = false;
    std::size_t shape_job_count = 0u;
    std::uint64_t shape_operation_identity = 0u;
    std::uint64_t job_submission_operation_identity = 0u;
    std::uint64_t job_committed_submissions = 0u;
    std::array<std::uint64_t, 4> job_submission_ordinals{};
    bool zero_anchor_sample_attempted = false;
    bool query_buffer_identity_matches = false;
    bool query_buffer_mapped_coherent = false;
    bool device_query_sample_available = false;
    std::uint32_t device_query_word_kind = 0u;
    std::uint32_t device_query_words32_per_record = 0u;
    std::uint64_t device_query_minimizer_index = 0u;
    std::array<std::uint32_t, 4> device_query_raw_words{};
    std::uint64_t device_query_decoded_hash = 0u;
    bool host_expected_hash_available = false;
    std::uint64_t host_expected_hash = 0u;
    bool device_query_matches_host_expected = false;
    bool resident_identity_matches = false;
    bool resident_aggregate_lookup_valid = false;
    std::uint64_t resident_aggregate_exact_count = 0u;
    bool resident_aggregate_occurrence_filtered = false;
    std::uint32_t resident_covering_partition_count = 0u;
    std::uint32_t resident_exact_hit_partition_count = 0u;
    bool resident_partition_selected = false;
    std::uint32_t resident_partition_ordinal = 0u;
    std::uint32_t resident_partition_directory_ordinal = 0u;
    std::uint32_t resident_partition_id = 0u;
    std::uint64_t resident_partition_low_hash = 0u;
    std::uint64_t resident_partition_high_hash = 0u;
    std::uint32_t resident_lower_bound_index = 0u;
    bool resident_lower_bound_value_available = false;
    std::uint64_t resident_lower_bound_value = 0u;
    bool resident_previous_value_available = false;
    std::uint64_t resident_previous_value = 0u;
    bool resident_exact_hit = false;
    bool resident_exact_control_tuple_valid = false;
    std::uint64_t resident_position_begin = 0u;
    std::uint64_t resident_position_end = 0u;
    std::uint64_t resident_saturated_count = 0u;
};
#endif

/// @brief Move-only accepted operation and its immutable environment identity.
class persisted_grouped_soa_coordinate_accepted_operation {
  public:
    /// @brief Constructs an empty invalid accepted operation.
    persisted_grouped_soa_coordinate_accepted_operation() = default;
    /// @brief Prevents copying accepted-operation authority.
    persisted_grouped_soa_coordinate_accepted_operation(
        const persisted_grouped_soa_coordinate_accepted_operation&) = delete;
    /// @brief Prevents copy-assignment of accepted-operation authority.
    persisted_grouped_soa_coordinate_accepted_operation&
    operator=(const persisted_grouped_soa_coordinate_accepted_operation&) = delete;
    /// @brief Transfers acceptance, environment, and cache-metric authority.
    persisted_grouped_soa_coordinate_accepted_operation(
        persisted_grouped_soa_coordinate_accepted_operation&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          environment_(std::exchange(other.environment_, {})),
          cache_metrics_(std::move(other.cache_metrics_)),
          issued_(std::exchange(other.issued_, false)) {}
    /// @brief Replaces this object with transferred accepted-operation authority.
    /// @return This accepted operation.
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

    /// @brief Validates the acceptance audit, environment, and metric delta.
    /// @return `true` when all accepted-operation evidence is usable.
    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && audit_.is_internally_consistent() &&
               environment_.is_internally_consistent() && cache_metrics_.is_valid();
    }
    /// @brief Returns immutable aggregate acceptance evidence.
    [[nodiscard]] const persisted_grouped_soa_coordinate_operation_acceptance_audit&
    audit() const noexcept {
        return audit_;
    }
    /// @brief Returns the immutable GPU execution-environment binding.
    [[nodiscard]] const runtime_gpu_execution_environment_snapshot&
    environment_binding() const noexcept {
        return environment_;
    }
    /// @brief Returns the immutable cache-metrics audit.
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& cache_metrics_audit() const noexcept {
        return cache_metrics_.audit();
    }
    /// @brief Transfers the one-shot cache-metrics delta to the caller.
    /// @return Move-owned cache-metrics delta.
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
    /// @brief Constructs an empty unsealed operation receipt.
    persisted_grouped_soa_coordinate_operation_receipt() = default;
    /// @brief Prevents copying operation-receipt authority.
    persisted_grouped_soa_coordinate_operation_receipt(
        const persisted_grouped_soa_coordinate_operation_receipt&) = delete;
    /// @brief Prevents copy-assignment of operation-receipt authority.
    persisted_grouped_soa_coordinate_operation_receipt&
    operator=(const persisted_grouped_soa_coordinate_operation_receipt&) = delete;
    /// @brief Transfers operation authority and invalidates the source.
    persisted_grouped_soa_coordinate_operation_receipt(
        persisted_grouped_soa_coordinate_operation_receipt&& other) noexcept {
        move_from(other);
    }
    /// @brief Releases current records, then transfers another operation receipt.
    /// @return This operation receipt.
    persisted_grouped_soa_coordinate_operation_receipt&
    operator=(persisted_grouped_soa_coordinate_operation_receipt&& other) noexcept {
        if (this != &other) {
            release_records();
            query_pipeline_token_ = {};
            grouped_soa_pipeline_token_ = {};
            alignment_pipeline_token_ = {};
            mapq_pipeline_token_ = {};
            mapq_result_count_ = 0u;
            mapq_published_count_ = 0u;
            mapq_result_digest_ = 0u;
            move_from(other);
        }
        return *this;
    }

    /// @brief Reports whether runtime sealed a nonzero operation identity.
    [[nodiscard]] bool is_sealed() const noexcept {
        return sealed_ && operation_identity_ != 0u;
    }
    /// @brief Returns the number of jobs in a sealed receipt.
    [[nodiscard]] std::size_t job_count() const noexcept {
        return is_sealed() ? records_.size() : 0u;
    }
    /// @brief Returns accepted plus burned jobs in a sealed receipt.
    [[nodiscard]] std::size_t consumed_count() const noexcept {
        return is_sealed() ? accepted_count_ + burned_count_ : 0u;
    }
    /// @brief Returns successfully accepted jobs in a sealed receipt.
    [[nodiscard]] std::size_t accepted_count() const noexcept {
        return is_sealed() ? accepted_count_ : 0u;
    }
    /// @brief Returns jobs permanently burned by failed validation.
    [[nodiscard]] std::size_t burned_count() const noexcept {
        return is_sealed() ? burned_count_ : 0u;
    }
    /// @brief Reports whether every job is accepted or burned.
    [[nodiscard]] bool is_fully_consumed() const noexcept {
        return is_sealed() && !records_.empty() &&
               std::all_of(records_.begin(), records_.end(), [](const auto& record) {
                   return record.state != record_state::fresh;
               });
    }
    /// @brief Reports whether every job was accepted with valid causal tokens.
    [[nodiscard]] bool are_all_jobs_accepted() const noexcept {
        return is_sealed() && is_fully_consumed() && accepted_count_ == records_.size() &&
               burned_count_ == 0u && has_valid_causal_tokens();
    }
    /// @brief Reports whether a causal packed-window alignment/CIGAR dispatch is attached.
    [[nodiscard]] bool has_alignment_cigar_receipt() const noexcept {
        if (!alignment_pipeline_token_.is_valid_for_persisted_alignment_cigar() ||
            alignment_pipeline_token_.audit().operation_identity != operation_identity_) {
            return false;
        }
        const auto& alignment_jobs = alignment_pipeline_token_.audit().jobs;
        if (!requires_mapq_receipt()) {
            std::size_t valid_coordinate_count = 0u;
            for (const record& coordinate_record : records_) {
                valid_coordinate_count += coordinate_record.audit.result.is_valid == 1u ? 1u : 0u;
            }
            if (alignment_jobs.empty() || alignment_jobs.size() != valid_coordinate_count) {
                return false;
            }
            std::uint64_t prior_problem_ordinal = 0u;
            for (const runtime_pipeline_job_use_audit& job : alignment_jobs) {
                if (job.anchor_count == 0u || job.anchor_count > records_.size() ||
                    job.anchor_count <= prior_problem_ordinal || job.range_count != 1u) {
                    return false;
                }
                const record& coordinate_record =
                    records_[static_cast<std::size_t>(job.anchor_count - 1u)];
                if (coordinate_record.state != record_state::accepted ||
                    coordinate_record.audit.result.is_valid != 1u) {
                    return false;
                }
                prior_problem_ordinal = job.anchor_count;
            }
            return true;
        }
        std::size_t retained_candidate_count = 0u;
        for (const record& coordinate_record : records_) {
            const auto& header = coordinate_record.audit.candidate_header;
            if (coordinate_record.state != record_state::accepted ||
                !coordinate_record.audit.configuration.mapq.is_valid() ||
                header.status != k_gpu_mapq_status_ok ||
                header.retained_count > header.candidate_count ||
                retained_candidate_count >
                    (std::numeric_limits<std::size_t>::max)() - header.retained_count) {
                return false;
            }
            retained_candidate_count += header.retained_count;
        }
        if (retained_candidate_count == 0u || alignment_jobs.size() != retained_candidate_count) {
            return false;
        }
        std::uint64_t prior_problem_ordinal = 0u;
        std::uint64_t prior_candidate_ordinal = 0u;
        for (std::size_t job_index = 0u; job_index < alignment_jobs.size(); ++job_index) {
            const std::uint64_t problem_ordinal = alignment_jobs[job_index].anchor_count;
            const std::uint64_t candidate_ordinal = alignment_jobs[job_index].range_count;
            if (problem_ordinal == 0u || problem_ordinal > records_.size() ||
                candidate_ordinal == 0u ||
                (job_index != 0u && (problem_ordinal < prior_problem_ordinal ||
                                     (problem_ordinal == prior_problem_ordinal &&
                                      candidate_ordinal <= prior_candidate_ordinal)))) {
                return false;
            }
            const record& coordinate_record =
                records_[static_cast<std::size_t>(problem_ordinal - 1u)];
            const auto& header = coordinate_record.audit.candidate_header;
            if (coordinate_record.state != record_state::accepted ||
                candidate_ordinal > header.candidate_count) {
                return false;
            }
            prior_problem_ordinal = problem_ordinal;
            prior_candidate_ordinal = candidate_ordinal;
        }
        std::size_t alignment_cursor = 0u;
        for (std::size_t problem_index = 0u; problem_index < records_.size(); ++problem_index) {
            const std::uint64_t problem_ordinal = static_cast<std::uint64_t>(problem_index) + 1u;
            std::uint64_t problem_jobs = 0u;
            while (alignment_cursor < alignment_jobs.size() &&
                   alignment_jobs[alignment_cursor].anchor_count == problem_ordinal) {
                ++problem_jobs;
                ++alignment_cursor;
            }
            if (problem_jobs != records_[problem_index].audit.candidate_header.retained_count) {
                return false;
            }
        }
        return alignment_cursor == alignment_jobs.size();
    }
    /// @brief Reports whether physical post-alignment MAPQ was selected for this operation.
    [[nodiscard]] bool requires_mapq_receipt() const noexcept {
        return !records_.empty() &&
               std::ranges::all_of(records_, [](const record& coordinate_record) {
                   return coordinate_record.audit.configuration.mapq.is_valid();
               });
    }
    /// @brief Reports whether one physical post-alignment MAPQ dispatch is attached.
    [[nodiscard]] bool has_mapq_receipt() const noexcept {
        if (!requires_mapq_receipt() || !mapq_pipeline_token_.is_valid_for_persisted_mapq() ||
            mapq_pipeline_token_.audit().operation_identity != operation_identity_ ||
            mapq_result_digest_ == 0u) {
            return false;
        }
        std::uint64_t candidate_count = 0u;
        std::uint64_t alignment_count = 0u;
        for (const record& coordinate_record : records_) {
            const auto& header = coordinate_record.audit.candidate_header;
            if (coordinate_record.state != record_state::accepted ||
                header.candidate_count >
                    (std::numeric_limits<std::uint64_t>::max)() - candidate_count ||
                header.retained_count >
                    (std::numeric_limits<std::uint64_t>::max)() - alignment_count) {
                return false;
            }
            candidate_count += header.candidate_count;
            alignment_count += header.retained_count;
        }
        const runtime_pipeline_job_use_audit& use = mapq_pipeline_token_.audit().jobs.front();
        return alignment_count <= candidate_count && mapq_result_count_ == candidate_count &&
               mapq_published_count_ <= alignment_count && use.minimizer_count == records_.size() &&
               use.target_len == candidate_count && use.target_position_offset == alignment_count &&
               use.anchor_count == mapq_published_count_ &&
               use.range_count == mapq_result_digest_ &&
               ((alignment_count == 0u && alignment_pipeline_token_.audit().kind ==
                                              runtime_pipeline_operation_kind::invalid) ||
                (alignment_count != 0u && has_alignment_cigar_receipt()));
    }
    /// @brief Derives the authenticated execution environment without consuming authority.
    [[nodiscard]] runtime_gpu_execution_environment_snapshot environment_preview() const noexcept {
        return preview_environment();
    }
    /// @brief Derives maximum cache metrics without consuming authority.
    [[nodiscard]] runtime_gpu_cache_metrics_audit cache_metrics_preview() const noexcept {
        return maximum_cache_metrics();
    }
    /// @brief Builds an allocation-free aggregate receipt preview.
    [[nodiscard]] persisted_grouped_soa_coordinate_operation_receipt_preview
    operation_receipt_preview() const noexcept {
        persisted_grouped_soa_coordinate_operation_receipt_preview result{};
        if (!are_all_jobs_accepted() || !query_pipeline_token_.is_valid_for_query_sketch() ||
            !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return result;
        }
        const runtime_pipeline_operation_audit& query = query_pipeline_token_.audit();
        const runtime_pipeline_operation_audit& shape = grouped_soa_pipeline_token_.audit();
        const bool alignment_present = has_alignment_cigar_receipt();
        const runtime_pipeline_operation_audit* alignment =
            alignment_present ? std::addressof(alignment_pipeline_token_.audit()) : nullptr;
        const bool mapq_required = requires_mapq_receipt();
        const bool mapq_present = has_mapq_receipt();
        const runtime_pipeline_operation_audit* mapq =
            mapq_present ? std::addressof(mapq_pipeline_token_.audit()) : nullptr;
        if (mapq_required != mapq_present ||
            (!mapq_required &&
             mapq_pipeline_token_.audit().kind != runtime_pipeline_operation_kind::invalid)) {
            return result;
        }
        if (query.jobs.size() != 1u || query.jobs.front().committed_submissions != 2u ||
            shape.committed_submissions >
                (std::numeric_limits<std::uint64_t>::max)() - query.committed_submissions) {
            return result;
        }
        const std::uint64_t coordinate_submissions =
            query.committed_submissions + shape.committed_submissions;
        if (alignment_present &&
            alignment->committed_submissions >
                (std::numeric_limits<std::uint64_t>::max)() - coordinate_submissions) {
            return result;
        }
        const std::uint64_t coordinate_alignment_submissions =
            coordinate_submissions + (alignment_present ? alignment->committed_submissions : 0u);
        if (mapq_present &&
            mapq->committed_submissions >
                (std::numeric_limits<std::uint64_t>::max)() - coordinate_alignment_submissions) {
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
            .alignment_pipeline_receipt_count = alignment_present ? 1u : 0u,
            .alignment_submission_count = alignment_present ? alignment->committed_submissions : 0u,
            .mapq_pipeline_receipt_count = mapq_present ? 1u : 0u,
            .mapq_submission_count = mapq_present ? mapq->committed_submissions : 0u,
            .mapq_result_count = mapq_present ? mapq_result_count_ : 0u,
            .mapq_published_count = mapq_present ? mapq_published_count_ : 0u,
            .mapq_result_digest = mapq_present ? mapq_result_digest_ : 0u,
            .committed_submission_count = coordinate_alignment_submissions +
                                          (mapq_present ? mapq->committed_submissions : 0u),
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
        if (query.cache_qualification != shape.cache_qualification ||
            (alignment_present && query.cache_qualification != alignment->cache_qualification) ||
            (mapq_present && query.cache_qualification != mapq->cache_qualification)) {
            return {};
        }
        result.cache_qualification = query.cache_qualification;
        result.maximum_cache_metrics = maximum_cache_metrics();
        return result.is_internally_consistent()
                   ? result
                   : persisted_grouped_soa_coordinate_operation_receipt_preview{};
    }
    /// @brief Builds full-stage evidence for one accepted job.
    /// @return Job evidence when the operation and submission record are valid.
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

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /// @brief Copies existing receipt fields without granting acceptance authority.
    /// @return Diagnostic values for the requested job; missing fields remain zero.
    [[nodiscard]] persisted_grouped_soa_coordinate_job_failure_diagnostic
    job_failure_diagnostic(const std::size_t problem_index) const noexcept {
        persisted_grouped_soa_coordinate_job_failure_diagnostic result{
            .operation_sealed = is_sealed(),
            .all_jobs_accepted = are_all_jobs_accepted(),
            .operation_job_count = records_.size(),
            .shape_pipeline_token_valid =
                grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate(),
        };
        if (problem_index < records_.size()) {
            result.audit = records_[problem_index].audit;
        }
        if (!result.shape_pipeline_token_valid) {
            return result;
        }
        const runtime_pipeline_operation_audit& shape = grouped_soa_pipeline_token_.audit();
        result.shape_job_count = shape.jobs.size();
        result.shape_operation_identity = shape.operation_identity;
        if (problem_index < shape.jobs.size()) {
            const runtime_pipeline_job_use_audit& use = shape.jobs[problem_index];
            result.job_submission_operation_identity = use.submission_operation_identity;
            result.job_committed_submissions = use.committed_submissions;
            result.job_submission_ordinals = use.submission_ordinals;
        }
        return result;
    }
#endif

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

    /// @brief Consumes one fresh job after exact identity validation.
    /// @return One-shot job receipt on acceptance; otherwise no value and the job is burned.
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
            are_chain_candidate_headers_equal(audit.candidate_header, expected.candidate_header) &&
            audit.candidate_digest == expected.candidate_digest &&
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

    /// @brief One-shot consumption state for an operation job record.
    enum class record_state : std::uint8_t {
        fresh,    ///< Available for its first validation attempt.
        accepted, ///< Successfully converted into a job receipt.
        burned,   ///< Permanently invalidated by failed validation.
    };

    /// @brief Sealed job audit and its one-shot consumption state.
    struct record {
        persisted_grouped_soa_coordinate_job_audit audit{};
        record_state state = record_state::fresh;
    };

    /// @brief Deduplicated pipeline receipts shared by query and shape tokens.
    struct pipeline_receipt_union {
        std::array<shader_variant_key, k_runtime_pipeline_prepared_slots> keys{};
        std::array<const runtime_metal_archive_pipeline_receipt*, k_runtime_pipeline_prepared_slots>
            receipts{};
        std::size_t count = 0u;
        bool valid = false;
    };

    /// @brief Merges distinct immutable receipts from query, shape, alignment, and MAPQ tokens.
    [[nodiscard]] pipeline_receipt_union receipt_union() const noexcept {
        pipeline_receipt_union result{};
        if (!sealed_ || test_causal_accepted_ ||
            !query_pipeline_token_.is_valid_for_query_sketch() ||
            !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return result;
        }
        // Merges one operation token while rejecting duplicate keys with different receipts.
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
        const bool alignment_present = has_alignment_cigar_receipt();
        const bool mapq_present = has_mapq_receipt();
        if (!merge(query_pipeline_token_) || !merge(grouped_soa_pipeline_token_) ||
            (alignment_present && !merge(alignment_pipeline_token_)) ||
            (mapq_present && !merge(mapq_pipeline_token_)) || result.count == 0u ||
            result.count != grouped_soa_pipeline_token_.audit_.prepared_count + 1u +
                                (alignment_present ? 1u : 0u) + (mapq_present ? 1u : 0u)) {
            return {};
        }
        result.valid = true;
        return result;
    }

    /// @brief Validates shared pipeline identity and constructs an environment snapshot.
    [[nodiscard]] runtime_gpu_execution_environment_snapshot preview_environment() const noexcept {
        const pipeline_receipt_union union_receipts = receipt_union();
        if (!union_receipts.valid)
            return {};
        const runtime_pipeline_operation_audit& query = query_pipeline_token_.audit_;
        const runtime_pipeline_operation_audit& shape = grouped_soa_pipeline_token_.audit_;
        const bool alignment_present = has_alignment_cigar_receipt();
        const runtime_pipeline_operation_audit* alignment =
            alignment_present ? std::addressof(alignment_pipeline_token_.audit_) : nullptr;
        const bool mapq_present = has_mapq_receipt();
        const runtime_pipeline_operation_audit* mapq =
            mapq_present ? std::addressof(mapq_pipeline_token_.audit_) : nullptr;
        if (query.device_uuid != shape.device_uuid || query.api_version != shape.api_version ||
            query.driver_version != shape.driver_version || query.vendor_id != shape.vendor_id ||
            query.device_id != shape.device_id ||
            query.pipeline_cache_uuid != shape.pipeline_cache_uuid ||
            query.seed_sha256 != shape.seed_sha256 || query.seed_size != shape.seed_size ||
            query.manifest_checksum != shape.manifest_checksum ||
            query.seed_receipt_identity != shape.seed_receipt_identity ||
            query.archive_set != shape.archive_set ||
            query.cache_qualification != shape.cache_qualification ||
            (alignment_present &&
             (query.device_uuid != alignment->device_uuid ||
              query.api_version != alignment->api_version ||
              query.driver_version != alignment->driver_version ||
              query.vendor_id != alignment->vendor_id || query.device_id != alignment->device_id ||
              query.pipeline_cache_uuid != alignment->pipeline_cache_uuid ||
              query.seed_sha256 != alignment->seed_sha256 ||
              query.seed_size != alignment->seed_size ||
              query.manifest_checksum != alignment->manifest_checksum ||
              query.seed_receipt_identity != alignment->seed_receipt_identity ||
              query.archive_set != alignment->archive_set ||
              query.cache_qualification != alignment->cache_qualification)) ||
            (mapq_present &&
             (query.device_uuid != mapq->device_uuid || query.api_version != mapq->api_version ||
              query.driver_version != mapq->driver_version || query.vendor_id != mapq->vendor_id ||
              query.device_id != mapq->device_id ||
              query.pipeline_cache_uuid != mapq->pipeline_cache_uuid ||
              query.seed_sha256 != mapq->seed_sha256 || query.seed_size != mapq->seed_size ||
              query.manifest_checksum != mapq->manifest_checksum ||
              query.seed_receipt_identity != mapq->seed_receipt_identity ||
              query.archive_set != mapq->archive_set ||
              query.cache_qualification != mapq->cache_qualification))) {
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

    /// @brief Aggregates unique pipeline creation metrics for this operation.
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

    /// @brief Admits and allocates the fixed one-shot record vector.
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

    /// @brief Releases record capacity before returning its admission permit.
    void release_records() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            records_, records_capacity_permit_);
    }

    /// @brief Validates query, grouped-SoA, and any attached downstream pipeline tokens.
    [[nodiscard]] bool has_valid_causal_tokens() const noexcept {
        return test_causal_accepted_ ||
               (query_pipeline_token_.is_valid_for_query_sketch() &&
                grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate() &&
                (alignment_pipeline_token_.audit().kind ==
                     runtime_pipeline_operation_kind::invalid ||
                 has_alignment_cigar_receipt()) &&
                (mapq_pipeline_token_.audit().kind == runtime_pipeline_operation_kind::invalid ||
                 has_mapq_receipt()));
    }

    /// @brief Transfers all receipt state and invalidates the source.
    void move_from(persisted_grouped_soa_coordinate_operation_receipt& other) noexcept {
        operation_identity_ = other.operation_identity_;
        records_ = std::move(other.records_);
        records_capacity_permit_ = std::move(other.records_capacity_permit_);
        query_pipeline_token_ = std::move(other.query_pipeline_token_);
        grouped_soa_pipeline_token_ = std::move(other.grouped_soa_pipeline_token_);
        alignment_pipeline_token_ = std::move(other.alignment_pipeline_token_);
        mapq_pipeline_token_ = std::move(other.mapq_pipeline_token_);
        mapq_result_count_ = std::exchange(other.mapq_result_count_, std::uint64_t{0});
        mapq_published_count_ = std::exchange(other.mapq_published_count_, std::uint64_t{0});
        mapq_result_digest_ = std::exchange(other.mapq_result_digest_, std::uint64_t{0});
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
    runtime_pipeline_operation_token alignment_pipeline_token_{};
    runtime_pipeline_operation_token mapq_pipeline_token_{};
    std::uint64_t mapq_result_count_ = 0u;
    std::uint64_t mapq_published_count_ = 0u;
    std::uint64_t mapq_result_digest_ = 0u;
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

    /// @brief Validates every prepared finalization component.
    /// @return `true` when the plan can be committed atomically.
    [[nodiscard]] bool is_valid() const noexcept {
        return valid && cache_claim_plan.is_valid() && environment.is_internally_consistent() &&
               acceptance.is_internally_consistent();
    }
};

static_assert(!std::is_copy_constructible_v<persisted_grouped_soa_coordinate_finalization_plan>);
static_assert(
    std::is_nothrow_move_constructible_v<persisted_grouped_soa_coordinate_finalization_plan>);

} // namespace runtime_detail

/// @brief Accepted source-direct environment plus report-once cache metrics.
class source_direct_coordinate_accepted_execution {
  public:
    /// @brief Constructs an empty invalid acceptance.
    source_direct_coordinate_accepted_execution() = default;
    /// @brief Prevents copying report-once metric authority.
    source_direct_coordinate_accepted_execution(
        const source_direct_coordinate_accepted_execution&) = delete;
    /// @brief Prevents copy assignment of report-once metric authority.
    source_direct_coordinate_accepted_execution&
    operator=(const source_direct_coordinate_accepted_execution&) = delete;
    /// @brief Transfers accepted environment and metric authority.
    source_direct_coordinate_accepted_execution(
        source_direct_coordinate_accepted_execution&&) noexcept = default;
    /// @brief Replaces this acceptance with transferred authority.
    source_direct_coordinate_accepted_execution&
    operator=(source_direct_coordinate_accepted_execution&&) noexcept = default;

    /// @brief Validates both immutable execution evidence components.
    /// @return Whether the acceptance may be published once.
    [[nodiscard]] bool is_valid() const noexcept {
        return environment_.is_internally_consistent() && cache_metrics_.is_valid();
    }
    /// @brief Returns the immutable GPU environment binding.
    [[nodiscard]] const runtime_gpu_execution_environment_snapshot&
    environment_binding() const noexcept {
        return environment_;
    }
    /// @brief Returns the exact report-once cache-metric audit.
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& cache_metrics_audit() const noexcept {
        return cache_metrics_.audit();
    }
    /// @brief Transfers cache-metric publication authority to the caller.
    [[nodiscard]] runtime_gpu_cache_metrics_delta take_cache_metrics_delta() noexcept {
        return std::move(cache_metrics_);
    }

  private:
    runtime_gpu_execution_environment_snapshot environment_{};
    runtime_gpu_cache_metrics_delta cache_metrics_{};
    friend class source_direct_coordinate_execution_receipt;
};

static_assert(!std::is_copy_constructible_v<source_direct_coordinate_accepted_execution>);
static_assert(std::is_nothrow_move_constructible_v<source_direct_coordinate_accepted_execution>);

/// @brief One-shot runtime authority for a completed source-direct coordinate batch.
///
/// Only runtime can seal this receipt. The retained stats and result wires bind the
/// GPU environment to the exact successful return value consumed by PostgreSQL.
class source_direct_coordinate_execution_receipt {
  public:
    /// @brief Constructs an empty, invalid receipt.
    source_direct_coordinate_execution_receipt() = default;
    /// @brief Releases retained result capacity before its admission permit.
    ~source_direct_coordinate_execution_receipt() noexcept {
        release_results();
    }
    /// @brief Prevents copying one-shot runtime authority.
    source_direct_coordinate_execution_receipt(const source_direct_coordinate_execution_receipt&) =
        delete;
    /// @brief Prevents copy assignment of one-shot runtime authority.
    source_direct_coordinate_execution_receipt&
    operator=(const source_direct_coordinate_execution_receipt&) = delete;
    /// @brief Transfers the sealed receipt and invalidates the source.
    source_direct_coordinate_execution_receipt(
        source_direct_coordinate_execution_receipt&& other) noexcept {
        move_from(other);
    }
    /// @brief Replaces this receipt with transferred one-shot authority.
    /// @return This receipt.
    source_direct_coordinate_execution_receipt&
    operator=(source_direct_coordinate_execution_receipt&& other) noexcept {
        if (this != &other) {
            release_results();
            move_from(other);
        }
        return *this;
    }

    /// @brief Reports whether runtime sealed nonempty source-direct GPU work.
    [[nodiscard]] bool is_sealed() const noexcept {
        return issued_ && environment_.is_internally_consistent() && cache_metrics_.is_valid() &&
               stats_.problem_count != 0u && stats_.wave_count != 0u &&
               stats_.seed_count_dispatches != 0u &&
               stats_.problem_count == static_cast<std::uint64_t>(results_.size());
    }

    /// @brief Consumes the receipt on exact completed-stats and result agreement.
    /// @param[in] stats Batch counters returned alongside this receipt.
    /// @param[in] results Result wires returned alongside this receipt.
    /// @return Causally sealed execution authority, or no value after mismatch/reuse.
    [[nodiscard]] std::optional<source_direct_coordinate_accepted_execution>
    consume(const gpu_seed_coordinate_batch_stats& stats,
            const std::span<const gpu_coordinate_result_wire> results) noexcept {
        bool results_match = results.size() == results_.size();
        for (std::size_t index = 0u; results_match && index < results.size(); ++index) {
            results_match = are_coordinate_result_wires_equal(results[index], results_[index]);
        }
        const bool accepted = is_sealed() && stats == stats_ && results_match;
        source_direct_coordinate_accepted_execution execution{};
        if (accepted) {
            execution.environment_ = environment_;
            execution.cache_metrics_ = std::move(cache_metrics_);
        } else {
            cache_metrics_ = {};
        }
        environment_ = {};
        stats_ = {};
        issued_ = false;
        release_results();
        return accepted ? std::optional<source_direct_coordinate_accepted_execution>{std::move(
                              execution)}
                        : std::nullopt;
    }

  private:
    friend class runtime;

    /// @brief Admits and seals the exact successful runtime return value.
    void seal(const runtime_gpu_execution_environment_snapshot& environment,
              runtime_gpu_cache_metrics_delta cache_metrics,
              const gpu_seed_coordinate_batch_stats& stats,
              const std::span<const gpu_coordinate_result_wire> results) {
        using bioseq::common::saturating_mul;
        if (issued_ || results_capacity_permit_ || !results_.empty() ||
            !environment.is_internally_consistent() || !cache_metrics.is_valid() ||
            stats.problem_count == 0u || stats.wave_count == 0u ||
            stats.seed_count_dispatches == 0u ||
            stats.problem_count != static_cast<std::uint64_t>(results.size()) ||
            results.size() > results_.max_size()) {
            throw vulkan_runtime_error(
                "source-direct coordinate execution receipt shape is invalid");
        }
        const std::uint64_t logical = saturating_mul(static_cast<std::uint64_t>(results.size()),
                                                     sizeof(gpu_coordinate_result_wire));
        const std::uint64_t peak =
            saturating_mul(logical, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (logical == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw vulkan_runtime_error(
                "source-direct coordinate execution receipt admission saturated");
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            peak, &failure);
        if (!permit) {
            throw vulkan_runtime_error(shader_cache_message(
                "source-direct coordinate execution receipt admission denied: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                " requested=",
                failure.requested_bytes));
        }
        try {
            results_.assign(results.begin(), results.end());
        } catch (const std::bad_alloc&) {
            std::vector<gpu_coordinate_result_wire> empty;
            results_.swap(empty);
            throw vulkan_runtime_error(
                "source-direct coordinate execution receipt allocation failed after admission");
        }
        const std::uint64_t retained = saturating_mul(
            static_cast<std::uint64_t>(results_.capacity()), sizeof(gpu_coordinate_result_wire));
        if (retained == (std::numeric_limits<std::uint64_t>::max)() || retained > permit->bytes()) {
            std::vector<gpu_coordinate_result_wire> empty;
            results_.swap(empty);
            throw vulkan_runtime_error(
                "source-direct coordinate execution receipt exceeded admitted capacity");
        }
        permit->shrink_to(retained);
        results_capacity_permit_ = std::move(*permit);
        environment_ = environment;
        cache_metrics_ = std::move(cache_metrics);
        stats_ = stats;
        issued_ = true;
    }

    /// @brief Releases result storage before its covering permit.
    void release_results() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            results_, results_capacity_permit_);
    }

    /// @brief Transfers every sealed field and clears the source.
    void move_from(source_direct_coordinate_execution_receipt& other) noexcept {
        results_ = std::move(other.results_);
        results_capacity_permit_ = std::move(other.results_capacity_permit_);
        environment_ = std::exchange(other.environment_, {});
        cache_metrics_ = std::move(other.cache_metrics_);
        stats_ = std::exchange(other.stats_, {});
        issued_ = std::exchange(other.issued_, false);
        other.results_.clear();
    }

    // Declared before results_: capacity is destroyed before permit release.
    bioseq::parallelism::memory_permit results_capacity_permit_{};
    std::vector<gpu_coordinate_result_wire> results_{};
    runtime_gpu_execution_environment_snapshot environment_{};
    runtime_gpu_cache_metrics_delta cache_metrics_{};
    gpu_seed_coordinate_batch_stats stats_{};
    bool issued_ = false;
};

static_assert(!std::is_copy_constructible_v<source_direct_coordinate_execution_receipt>);
static_assert(std::is_nothrow_move_constructible_v<source_direct_coordinate_execution_receipt>);

/// @brief One completed MPS query job's exact slice in the returned wire vector.
struct grouped_soa_seed_wire_job_span {
    std::uint32_t ordinal = 0u; ///< Zero-based query-record ordinal sealed by runtime.
    std::size_t offset = 0u;    ///< First wire owned by the job.
    std::size_t count = 0u;     ///< Number of wires owned by the job.
};
static_assert(std::is_trivially_copyable_v<grouped_soa_seed_wire_job_span>);

/// @brief Validates the complete ordered job partition of one MPS wire image.
[[nodiscard]] inline bool grouped_soa_seed_wire_jobs_cover(
    std::span<const grouped_soa_seed_wire_job_span> jobs,
    std::size_t anchor_count) noexcept {
    std::size_t prefix = 0u;
    for (std::size_t index = 0u; index < jobs.size(); ++index) {
        const grouped_soa_seed_wire_job_span& job = jobs[index];
        if (index > (std::numeric_limits<std::uint32_t>::max)() ||
            job.ordinal != static_cast<std::uint32_t>(index) || job.offset != prefix ||
            prefix > anchor_count || job.count > anchor_count - prefix) {
            return false;
        }
        prefix += job.count;
    }
    return prefix == anchor_count;
}

/**
 * @brief Move-only completed MPS seed-only result.
 *
 * `gpu_anchor_wire` remains a distinct Vulkan ABI record. A higher seeding
 * adapter authenticates it and constructs canonical chaining anchors field by
 * field. Each host vector has an adjacent independent proof: either an exact
 * retained permit or a completed process-footprint reflection transaction.
 */
struct grouped_soa_seed_anchor_result {
    // Each permit precedes only the allocation it covers. Reverse destruction
    // therefore releases jobs/anchors before their adjacent reservations.
    bioseq::parallelism::memory_permit host_anchor_permit{}; ///< Unreflected wire capacity.
    std::vector<gpu_anchor_wire> anchor_wires{};
    bioseq::parallelism::memory_permit host_job_permit{}; ///< Unreflected job capacity.
    std::vector<grouped_soa_seed_wire_job_span> jobs{};
    runtime_pipeline_operation_token seed_operation_receipt{};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    // Transfer the already-issued query proof after the seed consumer completes.
    // It is not another query operation or a serialized statistics payload.
    runtime_pipeline_operation_token query_operation_receipt{};
#endif
    /// Anchor capacity was reconciled into the serialized process-footprint delta.
    bool host_anchor_capacity_reflected = false;
    /// Job capacity was reconciled into the serialized process-footprint delta.
    bool host_job_capacity_reflected = false;

    grouped_soa_seed_anchor_result() = default;
    grouped_soa_seed_anchor_result(const grouped_soa_seed_anchor_result&) = delete;
    grouped_soa_seed_anchor_result& operator=(const grouped_soa_seed_anchor_result&) = delete;
    grouped_soa_seed_anchor_result(grouped_soa_seed_anchor_result&& other) noexcept {
        move_from(other);
    }
    grouped_soa_seed_anchor_result&
    operator=(grouped_soa_seed_anchor_result&& other) noexcept {
        if (this != &other) {
            release_host_capacity();
            seed_operation_receipt = runtime_pipeline_operation_token{};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            query_operation_receipt = runtime_pipeline_operation_token{};
#endif
            move_from(other);
        }
        return *this;
    }

    /// @brief Returns physical anchor-vector capacity covered by its proof.
    [[nodiscard]] std::uint64_t owned_anchor_capacity_bytes() const noexcept {
        if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
            constexpr std::uint64_t maximum =
                (std::numeric_limits<std::uint64_t>::max)();
            if (anchor_wires.capacity() > maximum) {
                return maximum;
            }
        }
        return bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(anchor_wires.capacity()),
            sizeof(gpu_anchor_wire));
    }

    /// @brief Returns physical job-vector capacity covered by its proof.
    [[nodiscard]] std::uint64_t owned_job_capacity_bytes() const noexcept {
        if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
            constexpr std::uint64_t maximum =
                (std::numeric_limits<std::uint64_t>::max)();
            if (jobs.capacity() > maximum) {
                return maximum;
            }
        }
        return bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(jobs.capacity()),
            sizeof(grouped_soa_seed_wire_job_span));
    }

  private:
    /// @brief Destroys each vector allocation before returning its reservation.
    void release_host_capacity() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            jobs, host_job_permit);
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            anchor_wires, host_anchor_permit);
        host_anchor_capacity_reflected = false;
        host_job_capacity_reflected = false;
    }

    /// @brief Transfers vectors while source proofs remain live, then moves proofs last.
    void move_from(grouped_soa_seed_anchor_result& other) noexcept {
        anchor_wires = std::move(other.anchor_wires);
        jobs = std::move(other.jobs);
        seed_operation_receipt = std::move(other.seed_operation_receipt);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        query_operation_receipt = std::move(other.query_operation_receipt);
#endif
        {
            std::vector<gpu_anchor_wire> empty_anchors;
            std::vector<grouped_soa_seed_wire_job_span> empty_jobs;
            other.anchor_wires.swap(empty_anchors);
            other.jobs.swap(empty_jobs);
        }
        host_anchor_permit = std::move(other.host_anchor_permit);
        host_job_permit = std::move(other.host_job_permit);
        host_anchor_capacity_reflected =
            std::exchange(other.host_anchor_capacity_reflected, false);
        host_job_capacity_reflected =
            std::exchange(other.host_job_capacity_reflected, false);
    }
};

static_assert(!std::is_copy_constructible_v<grouped_soa_seed_anchor_result>);
static_assert(std::is_nothrow_move_constructible_v<grouped_soa_seed_anchor_result>);
static_assert(std::is_nothrow_move_assignable_v<grouped_soa_seed_anchor_result>);

/// @brief Coordinate results and causal receipt produced by a batched GPU run.
struct gpu_seed_coordinate_batch_result {
    std::vector<gpu_coordinate_result_wire> results{};
    // Declared before both vector owners so their capacity is released first.
    bioseq::parallelism::memory_permit candidate_capacity_permit{};
    std::vector<gpu_chain_candidate_header_wire> candidate_headers{};
    std::vector<gpu_chain_candidate_wire> candidates{};
    gpu_seed_coordinate_batch_stats stats{};
    persisted_grouped_soa_coordinate_operation_receipt operation_receipt{};
    source_direct_coordinate_execution_receipt source_direct_execution_receipt{};
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

    gpu_seed_coordinate_batch_result() = default;
    gpu_seed_coordinate_batch_result(const gpu_seed_coordinate_batch_result&) = delete;
    gpu_seed_coordinate_batch_result& operator=(const gpu_seed_coordinate_batch_result&) = delete;
    gpu_seed_coordinate_batch_result(gpu_seed_coordinate_batch_result&& other) noexcept
        : results(std::move(other.results)),
          candidate_capacity_permit(std::move(other.candidate_capacity_permit)),
          candidate_headers(std::move(other.candidate_headers)),
          candidates(std::move(other.candidates)), stats(std::exchange(other.stats, {})),
          operation_receipt(std::move(other.operation_receipt)),
          source_direct_execution_receipt(std::move(other.source_direct_execution_receipt)),
          grouped_soa_direct(std::exchange(other.grouped_soa_direct, {})) {}
    gpu_seed_coordinate_batch_result& operator=(gpu_seed_coordinate_batch_result&& other) noexcept {
        if (this != &other) {
            // Both candidate vectors release their capacity while the old shared permit is live.
            std::vector<gpu_chain_candidate_header_wire>{}.swap(candidate_headers);
            std::vector<gpu_chain_candidate_wire>{}.swap(candidates);
            candidate_capacity_permit = {};
            results = std::move(other.results);
            candidate_capacity_permit = std::move(other.candidate_capacity_permit);
            candidate_headers = std::move(other.candidate_headers);
            candidates = std::move(other.candidates);
            stats = std::exchange(other.stats, {});
            operation_receipt = std::move(other.operation_receipt);
            source_direct_execution_receipt = std::move(other.source_direct_execution_receipt);
            grouped_soa_direct = std::exchange(other.grouped_soa_direct, {});
        }
        return *this;
    }
};

static_assert(!std::is_copy_constructible_v<gpu_seed_coordinate_batch_result>);
static_assert(std::is_nothrow_move_constructible_v<gpu_seed_coordinate_batch_result>);
static_assert(std::is_nothrow_move_assignable_v<gpu_seed_coordinate_batch_result>);

} // namespace bioseq::gpu::vulkan

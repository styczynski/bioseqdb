/**
 * @file pipeline_stats.cppm
 * @brief Target-independent pipeline execution attestation contract.
 */
module;
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.pipeline.stats;

import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.minimizer_hashmap;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.pipeline.runtime_contract;
import bioseq.prefilter.execution_contract;

export namespace bioseq::pipeline_stats {

/// Requested or actually executed biological compute backend.
enum class executed_backend : std::uint8_t {
    none,
    scalar_cpu,
    simd_cpu,
    vulkan_gpu,
    mixed_invalid,
};

/// Proven origin of the target minimizer index consumed by a job.
enum class target_index_source : std::uint8_t {
    none,
    recomputed,
    persisted,
    mixed_invalid,
};

/// Statement-stable identity for the requested and operation-observed SIMD
/// chaining policy. This deliberately has its own none/mixed sentinels rather
/// than overloading the algorithm enum: pipeline aggregation must distinguish
/// "no SIMD policy applies" from "two concrete policy identities were mixed".
enum class simd_chaining_policy_identity : std::uint8_t {
    none,
    exact_visited,
    bounded_batch_approximate,
    mixed_invalid,
};

/// Statement-stable identity for the SIMD base-alignment algorithm.  This is
/// intentionally narrower than base_alignment_algorithm: the SIMD stats
/// object only needs to distinguish an authentic Block Aligner execution from
/// absence or a mixed/forbidden alignment route.
enum class simd_alignment_algorithm_identity : std::uint8_t {
    none,
    block_aligner,
    mixed_invalid,
};

/// Stable public spelling used by PostgreSQL and reporting. Keep this on the
/// aggregate identity rather than translating through the algorithm-local enum:
/// `none` and `mixed_invalid` are statement-level states that the local policy
/// type deliberately cannot represent.
[[nodiscard]] constexpr std::string_view simd_chaining_policy_identity_name(
    simd_chaining_policy_identity policy) noexcept
{
    switch (policy) {
        case simd_chaining_policy_identity::none:
            return "none";
        case simd_chaining_policy_identity::exact_visited:
            return "exact_visited";
        case simd_chaining_policy_identity::bounded_batch_approximate:
            return "bounded_batch_approximate";
        case simd_chaining_policy_identity::mixed_invalid:
            return "mixed_invalid";
    }
    return "mixed_invalid";
}

/// Return the stable report name of a SIMD alignment identity.
[[nodiscard]] constexpr std::string_view simd_alignment_algorithm_identity_name(
    simd_alignment_algorithm_identity algorithm) noexcept
{
    switch (algorithm) {
        case simd_alignment_algorithm_identity::none:
            return "none";
        case simd_alignment_algorithm_identity::block_aligner:
            return "block_aligner";
        case simd_alignment_algorithm_identity::mixed_invalid:
            return "mixed_invalid";
    }
    return "mixed_invalid";
}

/// Mutually exclusive engine credited with executing one pipeline stage.
enum class execution_engine : std::uint8_t {
    host_io,
    scalar_cpu,
    simd_cpu,
    vulkan_gpu,
    count,
};

/// Stable SQL/report ordinal for every attested pipeline stage.
enum class pipeline_stage : std::uint8_t {
    query_sketch = 0,
    target_sketch = 1,
    target_index_build = 2,
    persisted_index_decode = 3,
    partition_selection = 4,
    device_index_upload = 5,
    seed_lookup = 6,
    chain_score = 7,
    chain_backtrack = 8,
    region_generation = 9,
    primary_secondary_reduction = 10,
    divergence_estimation = 11,
    mapq = 12,
    target_window_plan = 13,
    target_window_io = 14,
    target_window_upload = 15,
    cigar_job_generation = 16,
    left_extension = 17,
    gap_extension = 18,
    right_extension = 19,
    count = 20,
};

/// Number of real execution engines, excluding the count sentinel.
inline constexpr std::size_t k_execution_engine_count =
    static_cast<std::size_t>(execution_engine::count);
/// Number of stable pipeline-stage ordinals, excluding the count sentinel.
inline constexpr std::size_t k_pipeline_stage_count =
    static_cast<std::size_t>(pipeline_stage::count);
/// Sentinel used when no failing target/query ordinal has been recorded.
inline constexpr std::uint64_t k_unset_job_ordinal =
    (std::numeric_limits<std::uint64_t>::max)();
/// Largest value representable by SQL bigint without lossy conversion.
inline constexpr std::uint64_t k_sql_bigint_value_max =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
/// Largest worker count representable by SQL integer.
inline constexpr std::uint32_t k_sql_integer_value_max =
    static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());

/// Return the stable report name for a biological backend value.
[[nodiscard]] constexpr std::string_view executed_backend_name(
    executed_backend backend) noexcept {
    switch (backend) {
        case executed_backend::none: return "none";
        case executed_backend::scalar_cpu: return "scalar_cpu";
        case executed_backend::simd_cpu: return "simd_cpu";
        case executed_backend::vulkan_gpu: return "vulkan_gpu";
        case executed_backend::mixed_invalid: return "mixed_invalid";
    }
    return "unknown";
}

/// Return the stable report name for a target-index source value.
[[nodiscard]] constexpr std::string_view target_index_source_name(
    target_index_source source) noexcept {
    switch (source) {
        case target_index_source::none: return "none";
        case target_index_source::recomputed: return "recomputed";
        case target_index_source::persisted: return "persisted";
        case target_index_source::mixed_invalid: return "mixed_invalid";
    }
    return "unknown";
}

/// Return the stable report name for an execution engine value.
[[nodiscard]] constexpr std::string_view execution_engine_name(
    execution_engine engine) noexcept {
    switch (engine) {
        case execution_engine::host_io: return "host_io";
        case execution_engine::scalar_cpu: return "scalar_cpu";
        case execution_engine::simd_cpu: return "simd_cpu";
        case execution_engine::vulkan_gpu: return "vulkan_gpu";
        case execution_engine::count: return "count";
    }
    return "unknown";
}

/// Return the stable report/SRF name for a pipeline-stage ordinal.
[[nodiscard]] constexpr std::string_view pipeline_stage_name(
    pipeline_stage stage) noexcept {
    switch (stage) {
        case pipeline_stage::query_sketch: return "query_sketch";
        case pipeline_stage::target_sketch: return "target_sketch";
        case pipeline_stage::target_index_build: return "target_index_build";
        case pipeline_stage::persisted_index_decode: return "persisted_index_decode";
        case pipeline_stage::partition_selection: return "partition_selection";
        case pipeline_stage::device_index_upload: return "device_index_upload";
        case pipeline_stage::seed_lookup: return "seed_lookup";
        case pipeline_stage::chain_score: return "chain_score";
        case pipeline_stage::chain_backtrack: return "chain_backtrack";
        case pipeline_stage::region_generation: return "region_generation";
        case pipeline_stage::primary_secondary_reduction: return "primary_secondary_reduction";
        case pipeline_stage::divergence_estimation: return "divergence_estimation";
        case pipeline_stage::mapq: return "mapq";
        case pipeline_stage::target_window_plan: return "target_window_plan";
        case pipeline_stage::target_window_io: return "target_window_io";
        case pipeline_stage::target_window_upload: return "target_window_upload";
        case pipeline_stage::cigar_job_generation: return "cigar_job_generation";
        case pipeline_stage::left_extension: return "left_extension";
        case pipeline_stage::gap_extension: return "gap_extension";
        case pipeline_stage::right_extension: return "right_extension";
        case pipeline_stage::count: return "count";
    }
    return "unknown";
}

/// Convert a validated engine enum to its coverage-array index.
[[nodiscard]] constexpr std::size_t engine_index(execution_engine engine) noexcept {
    return static_cast<std::size_t>(engine);
}

/// Convert a validated stage enum to its coverage-array index.
[[nodiscard]] constexpr std::size_t stage_index(pipeline_stage stage) noexcept {
    return static_cast<std::size_t>(stage);
}

/// Merge a SQL-bound count/time/byte value with the project's shared
/// saturating arithmetic. PostgreSQL bigint is signed, so values clamp at
/// INT64_MAX. An exact sum of INT64_MAX is representable and is not saturation.
[[nodiscard]] inline bool merge_counter(
    std::uint64_t& destination,
    std::uint64_t increment) noexcept {
    if (destination > k_sql_bigint_value_max) {
        destination = k_sql_bigint_value_max;
        return true;
    }
    if (increment > k_sql_bigint_value_max - destination) {
        destination = k_sql_bigint_value_max;
        return true;
    }
    destination = bioseq::common::saturating_add(destination, increment);
    return false;
}

/// Target-independent projection of the operation-local Block Aligner stats.
/// Every field is copied at the dispatch boundary from
/// block_aligner_execution_stats. Stage coverage and generic SIMD counters are
/// never used to manufacture these statistics.
struct block_aligner_stage_stats {
    bool is_available = false;
    bool is_accepted = false;
    std::uint64_t executed_calls = 0;
    std::uint64_t nonempty_calls = 0;
    std::uint64_t nonempty_calls_with_simd = 0;
    std::uint64_t simd_dp_vector_batches = 0;
    std::uint64_t simd_dp_cells = 0;
    std::uint64_t forbidden_ksw2_calls = 0;
    std::uint64_t forbidden_scalar_biological_fallbacks = 0;
    bool is_saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const block_aligner_stage_stats&) const noexcept = default;

    /// Merge independently captured operation statistics. Every available
    /// operand must be accepted and SQL-bound counter overflow revokes it.
    [[nodiscard]] bool merge(
        const block_aligner_stage_stats& rhs) noexcept {
        if (!rhs.is_available) {
            const bool malformed_absence = rhs.is_accepted
                || rhs.executed_calls != 0
                || rhs.nonempty_calls != 0
                || rhs.nonempty_calls_with_simd != 0
                || rhs.simd_dp_vector_batches != 0
                || rhs.simd_dp_cells != 0
                || rhs.forbidden_ksw2_calls != 0
                || rhs.forbidden_scalar_biological_fallbacks != 0
                || rhs.is_saturated;
            if (malformed_absence) {
                is_saturated = true;
                is_accepted = false;
            }
            return is_saturated;
        }

        if (!is_available) {
            is_available = true;
            is_accepted = rhs.is_accepted;
        } else {
            is_accepted = is_accepted && rhs.is_accepted;
        }
        bool overflow = is_saturated || rhs.is_saturated;
        overflow = merge_counter(executed_calls, rhs.executed_calls)
            || overflow;
        overflow = merge_counter(nonempty_calls, rhs.nonempty_calls)
            || overflow;
        overflow = merge_counter(
            nonempty_calls_with_simd, rhs.nonempty_calls_with_simd)
            || overflow;
        overflow = merge_counter(
            simd_dp_vector_batches, rhs.simd_dp_vector_batches)
            || overflow;
        overflow = merge_counter(simd_dp_cells, rhs.simd_dp_cells)
            || overflow;
        overflow = merge_counter(
            forbidden_ksw2_calls, rhs.forbidden_ksw2_calls)
            || overflow;
        overflow = merge_counter(
            forbidden_scalar_biological_fallbacks,
            rhs.forbidden_scalar_biological_fallbacks)
            || overflow;
        is_saturated = overflow;
        if (is_saturated) {
            is_accepted = false;
        }
        return is_saturated;
    }
};

/// Count distinct logical target-row identities in-place. Physical payload
/// coalescing is deliberately irrelevant: two relation rows with identical
/// sequence bytes remain two logical targets, while repeated observations of
/// the same row identity collapse to one target outcome.
[[nodiscard]] inline std::uint64_t count_unique_logical_target_rows(
        std::span<std::uint64_t> logical_rows) {
    std::sort(logical_rows.begin(), logical_rows.end());
    const auto unique_end = std::unique(
        logical_rows.begin(), logical_rows.end());
    return static_cast<std::uint64_t>(unique_end - logical_rows.begin());
}

/// Binary per-job coverage or its saturating statement aggregate.
struct stage_coverage {
    std::uint64_t eligible_jobs = 0;
    std::array<std::uint64_t, k_execution_engine_count> executed_jobs{};
    std::uint64_t fallback_jobs = 0;

    [[nodiscard]] constexpr bool operator==(
        const stage_coverage&) const noexcept = default;

    /// Saturating merge. True means at least one SQL-bound count overflowed.
    [[nodiscard]] bool merge(const stage_coverage& rhs) noexcept {
        bool is_saturated = merge_counter(eligible_jobs, rhs.eligible_jobs);
        for (std::size_t i = 0; i < executed_jobs.size(); ++i) {
            is_saturated = merge_counter(executed_jobs[i], rhs.executed_jobs[i]) || is_saturated;
        }
        is_saturated = merge_counter(fallback_jobs, rhs.fallback_jobs) || is_saturated;
        return is_saturated;
    }
};

/// Coverage for every stable pipeline-stage ordinal.
struct pipeline_job_stage_coverage {
    std::array<stage_coverage, k_pipeline_stage_count> coverage{};

    [[nodiscard]] constexpr bool operator==(
        const pipeline_job_stage_coverage&) const noexcept = default;

    /// Merge all stage coverages and report whether any count is_saturated.
    [[nodiscard]] bool merge(const pipeline_job_stage_coverage& rhs) noexcept {
        bool is_saturated = false;
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            is_saturated = coverage[i].merge(rhs.coverage[i]) || is_saturated;
        }
        return is_saturated;
    }
};

/// Host and device time attributed to one pipeline stage.
struct pipeline_stage_timing {
    std::uint64_t host_wall_ns = 0;
    std::uint64_t gpu_device_ns = 0;

    [[nodiscard]] constexpr bool operator==(
        const pipeline_stage_timing&) const noexcept = default;
};

/// Merge a lossless u64 identity that may be published exactly once.
inline void merge_report_once_identity(
    std::uint64_t& destination,
    bool& destination_available,
    std::uint64_t incoming,
    bool incoming_available,
    bool& has_identity_conflict) noexcept {
    if ((!destination_available && destination != 0)
        || (!incoming_available && incoming != 0)) {
        has_identity_conflict = true;
    }
    if (!incoming_available) return;
    if (!destination_available) {
        destination = incoming;
        destination_available = true;
        return;
    }
    has_identity_conflict = true;
}

/// Hash-major cold-reader identities and counters for operational aggregation.
/// Identity fields are merged only when identical. Counters use the same
/// saturating arithmetic as the rest of the operational snapshot.
struct hash_major_index_reader_stats {
    std::uint64_t layout_revision = 0;
    std::uint64_t sessions = 0;
    bool identity_available = false;
    std::uint64_t generation_id = 0;
    std::array<std::byte, 16> source_identity{};
    std::uint64_t source_content_crc64 = 0;
    std::array<std::byte, 32> storage_contract_digest{};
    std::uint64_t directory_rows = 0;
    std::uint64_t directory_bytes = 0;
    std::uint64_t control_rows = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t posting_rows = 0;
    std::uint64_t posting_bytes = 0;
    std::uint64_t directory_row_crc_receipts = 0;
    std::uint64_t directory_stream_crc_receipts = 0;
    std::uint64_t control_row_crc_receipts = 0;
    std::uint64_t control_block_crc_receipts = 0;
    std::uint64_t control_semantic_receipts = 0;
    std::uint64_t posting_row_crc_receipts = 0;
    std::uint64_t posting_ranges_delivered = 0;
    std::uint64_t posting_range_crc_receipts = 0;
    std::uint64_t posting_semantic_receipts = 0;
    std::uint64_t authorized_posting_bytes = 0;
    std::uint64_t query_count = 0;
    std::uint64_t selected_block_count = 0;
    std::uint64_t accepted_hash_count = 0;
    std::uint64_t retained_position_count = 0;
    std::uint64_t directory_peak_bytes = 0;
    std::uint64_t control_peak_bytes = 0;
    std::uint64_t posting_peak_bytes = 0;
    std::uint64_t peak_admitted_bytes = 0;
    bool owner_bytes_available = false;
    std::uint64_t retained_owner_bytes = 0;
    std::uint64_t admitted_bytes = 0;
    std::uint64_t generic_source_fallback_calls = 0;
    std::uint64_t failures = 0;
    bool is_saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const hash_major_index_reader_stats&) const noexcept = default;

    [[nodiscard]] bool merge(
            const hash_major_index_reader_stats& rhs,
            bool& has_identity_conflict) noexcept {
        bool overflow = is_saturated || rhs.is_saturated;
        const auto add = [&overflow](
                std::uint64_t& destination,
                const std::uint64_t increment) noexcept {
            overflow = merge_counter(destination, increment) || overflow;
        };
        if (rhs.layout_revision != 0u) {
            if (layout_revision == 0u) {
                layout_revision = rhs.layout_revision;
            } else if (layout_revision != rhs.layout_revision) {
                has_identity_conflict = true;
            }
        }
        const bool destination_identity_payload =
            generation_id != 0u
            || source_identity != std::array<std::byte, 16>{}
            || source_content_crc64 != 0u
            || storage_contract_digest != std::array<std::byte, 32>{};
        const bool incoming_identity_payload =
            rhs.generation_id != 0u
            || rhs.source_identity != std::array<std::byte, 16>{}
            || rhs.source_content_crc64 != 0u
            || rhs.storage_contract_digest != std::array<std::byte, 32>{};
        if ((!identity_available && destination_identity_payload)
            || (!rhs.identity_available && incoming_identity_payload)) {
            has_identity_conflict = true;
        }
        if (rhs.identity_available) {
            if (!identity_available) {
                identity_available = true;
                generation_id = rhs.generation_id;
                source_identity = rhs.source_identity;
                source_content_crc64 = rhs.source_content_crc64;
                storage_contract_digest = rhs.storage_contract_digest;
            } else if (generation_id != rhs.generation_id
                || source_identity != rhs.source_identity
                || source_content_crc64 != rhs.source_content_crc64
                || storage_contract_digest != rhs.storage_contract_digest) {
                has_identity_conflict = true;
            }
        }
        const bool destination_owner_payload =
            retained_owner_bytes != 0u || admitted_bytes != 0u;
        const bool incoming_owner_payload =
            rhs.retained_owner_bytes != 0u || rhs.admitted_bytes != 0u;
        if ((!owner_bytes_available && destination_owner_payload)
            || (!rhs.owner_bytes_available && incoming_owner_payload)) {
            has_identity_conflict = true;
        }
        if (rhs.owner_bytes_available) {
            if (!owner_bytes_available) {
                owner_bytes_available = true;
                retained_owner_bytes = rhs.retained_owner_bytes;
                admitted_bytes = rhs.admitted_bytes;
            } else if (retained_owner_bytes != rhs.retained_owner_bytes
                || admitted_bytes != rhs.admitted_bytes) {
                has_identity_conflict = true;
            }
        }
        add(sessions, rhs.sessions);
        add(directory_rows, rhs.directory_rows);
        add(directory_bytes, rhs.directory_bytes);
        add(control_rows, rhs.control_rows);
        add(control_bytes, rhs.control_bytes);
        add(posting_rows, rhs.posting_rows);
        add(posting_bytes, rhs.posting_bytes);
        add(directory_row_crc_receipts,
            rhs.directory_row_crc_receipts);
        add(directory_stream_crc_receipts,
            rhs.directory_stream_crc_receipts);
        add(control_row_crc_receipts, rhs.control_row_crc_receipts);
        add(control_block_crc_receipts,
            rhs.control_block_crc_receipts);
        add(control_semantic_receipts,
            rhs.control_semantic_receipts);
        add(posting_row_crc_receipts, rhs.posting_row_crc_receipts);
        add(posting_ranges_delivered, rhs.posting_ranges_delivered);
        add(posting_range_crc_receipts,
            rhs.posting_range_crc_receipts);
        add(posting_semantic_receipts,
            rhs.posting_semantic_receipts);
        add(authorized_posting_bytes, rhs.authorized_posting_bytes);
        add(query_count, rhs.query_count);
        add(selected_block_count, rhs.selected_block_count);
        add(accepted_hash_count, rhs.accepted_hash_count);
        add(retained_position_count, rhs.retained_position_count);
        directory_peak_bytes = std::max(
            directory_peak_bytes,
            rhs.directory_peak_bytes);
        control_peak_bytes = std::max(
            control_peak_bytes,
            rhs.control_peak_bytes);
        posting_peak_bytes = std::max(
            posting_peak_bytes,
            rhs.posting_peak_bytes);
        peak_admitted_bytes = std::max(
            peak_admitted_bytes,
            rhs.peak_admitted_bytes);
        add(generic_source_fallback_calls,
            rhs.generic_source_fallback_calls);
        add(failures, rhs.failures);
        is_saturated = overflow;
        return overflow;
    }
};

/// Stable result code shared by grouped-SoA mapped-sink preallocation and
/// mapping-advice receipts. These values are report ABI, not raw errno values.
enum class grouped_soa_operation_result : std::uint64_t {
    not_attempted = 0u,
    succeeded = 1u,
    failed = 2u,
};

/// Stable identity of the selected lazy extent-validation state layout.
enum class grouped_soa_state_layout : std::uint64_t {
    unselected = 0u,
    compact_aligned_planes = 1u,
    cache_line_padded_entries = 2u,
};

/// Stable identity of the selected read-only mapping advice policy.
enum class grouped_soa_mapping_advice : std::uint64_t {
    none = 0u,
    random = 1u,
};

static_assert(static_cast<std::uint64_t>(
    grouped_soa_operation_result::not_attempted) == 0u);
static_assert(static_cast<std::uint64_t>(
    grouped_soa_operation_result::succeeded) == 1u);
static_assert(static_cast<std::uint64_t>(
    grouped_soa_operation_result::failed) == 2u);
static_assert(static_cast<std::uint64_t>(
    grouped_soa_state_layout::unselected) == 0u);
static_assert(static_cast<std::uint64_t>(
    grouped_soa_state_layout::compact_aligned_planes) == 1u);
static_assert(static_cast<std::uint64_t>(
    grouped_soa_state_layout::cache_line_padded_entries) == 2u);
static_assert(static_cast<std::uint64_t>(
    grouped_soa_mapping_advice::none) == 0u);
static_assert(static_cast<std::uint64_t>(
    grouped_soa_mapping_advice::random) == 1u);

/// Provider, mapped-image and lifecycle statistics appended by the exact
/// Grouped-SoA provider stats. Counter producers build operation-local
/// deltas through record_counter(). Aggregation remains checked and clamps at
/// SQL bigint max. Stable codes use explicit presence bits because provider
/// Code zero identifies the validated relation-backed storage provider.
struct grouped_soa_provider_stats {
    std::uint64_t provider_configured = 0u;
    std::uint64_t provider_captured = 0u;
    std::uint64_t provider_persisted = 0u;
    std::uint64_t provider_opened = 0u;
    std::uint64_t provider_head_scans = 0u;
    std::uint64_t provider_head_rows = 0u;
    std::uint64_t provider_descriptor_scans = 0u;
    std::uint64_t provider_descriptor_rows = 0u;

    std::uint64_t payload_tuples_formed = 0u;
    std::uint64_t payload_tuples_inserted = 0u;
    std::uint64_t payload_tuples_reread = 0u;
    std::uint64_t mapped_sink_calls = 0u;
    std::uint64_t mapped_sink_bytes = 0u;
    std::uint64_t mapped_sink_flushes = 0u;
    std::uint64_t mapped_sink_coalescer_capacity_bytes = 0u;
    std::uint64_t mapped_sink_pwrite_calls = 0u;
    std::uint64_t mapped_sink_short_writes = 0u;
    std::uint64_t mapped_sink_eintr_retries = 0u;
    std::uint64_t mapped_sink_preallocation_bytes = 0u;
    std::uint64_t mapped_sink_preallocation_result = 0u;

    std::uint64_t header_crc_bytes = 0u;
    std::uint64_t directory_crc_bytes = 0u;
    std::uint64_t manifest_crc_bytes = 0u;
    std::uint64_t seal_aggregate_payload_crc_bytes = 0u;
    std::uint64_t open_aggregate_payload_crc_bytes = 0u;

    std::uint64_t lazy_extent_cas_winners = 0u;
    std::uint64_t lazy_extent_waiters = 0u;
    std::uint64_t lazy_extent_executions = 0u;
    std::uint64_t lazy_extent_reuses = 0u;
    std::uint64_t lazy_extent_crc_bytes = 0u;
    std::uint64_t lazy_extent_state_bytes = 0u;
    std::uint64_t lazy_extent_is_invalid_receipts = 0u;
    std::uint64_t lazy_state_layout = 0u;
    std::uint64_t mapping_bytes = 0u;
    std::uint64_t mapping_count = 0u;
    std::uint64_t mapping_resourceowner_releases = 0u;

    std::uint64_t file_action_high_water = 0u;
    std::uint64_t file_action_identity_mismatches = 0u;
    std::uint64_t reaper_descriptors_examined = 0u;
    std::uint64_t reaper_relation_rows_deleted = 0u;
    std::uint64_t reaper_payload_bytes_deleted = 0u;
    std::uint64_t reaper_files_queued = 0u;
    std::uint64_t reaper_remaining = 0u;
    std::uint64_t orphan_entries_examined = 0u;
    std::uint64_t orphan_files_acted = 0u;
    std::uint64_t orphan_bytes_acted = 0u;
    std::uint64_t orphan_remaining = 0u;
    std::uint64_t orphan_cursor_wraps = 0u;

    std::uint64_t madvise_choice = 0u;
    std::uint64_t madvise_result = 0u;

    // Presence/conflict state is intentionally not serialized. It makes
    // report-once code zero distinguishable from an unrecorded field.
    bool provider_configured_available = false;
    bool provider_captured_available = false;
    bool provider_persisted_available = false;
    bool provider_opened_available = false;
    bool mapped_sink_preallocation_result_available = false;
    bool lazy_state_layout_available = false;
    bool madvise_choice_available = false;
    bool madvise_result_available = false;
    bool has_stable_value_conflict = false;
    bool is_saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const grouped_soa_provider_stats&) const noexcept = default;

    /// Add one SQL-bound count/byte receipt and retain a local saturation bit.
    void record_counter(
            std::uint64_t& destination,
            std::uint64_t increment) noexcept {
        is_saturated = merge_counter(destination, increment) || is_saturated;
    }

    /// File-action occupancy is a high-water mark, not an additive count.
    void record_file_action_high_water(std::uint64_t value) noexcept {
        if (value > k_sql_bigint_value_max) {
            value = k_sql_bigint_value_max;
            is_saturated = true;
        }
        file_action_high_water = std::max(file_action_high_water, value);
    }

    void record_provider_configured(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_configured,
            provider_configured_available,
            provider,
            has_stable_value_conflict);
    }

    void record_provider_captured(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_captured,
            provider_captured_available,
            provider,
            has_stable_value_conflict);
    }

    void record_provider_persisted(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_persisted,
            provider_persisted_available,
            provider,
            has_stable_value_conflict);
    }

    void record_provider_opened(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_opened,
            provider_opened_available,
            provider,
            has_stable_value_conflict);
    }

    void record_mapped_sink_preallocation_result(
            grouped_soa_operation_result result) noexcept {
        record_stable_value(
            mapped_sink_preallocation_result,
            mapped_sink_preallocation_result_available,
            static_cast<std::uint64_t>(result),
            has_stable_value_conflict);
    }

    void record_lazy_state_layout(grouped_soa_state_layout layout) noexcept {
        record_stable_value(
            lazy_state_layout,
            lazy_state_layout_available,
            static_cast<std::uint64_t>(layout),
            has_stable_value_conflict);
    }

    void record_madvise(
            grouped_soa_mapping_advice choice,
            grouped_soa_operation_result result) noexcept {
        record_stable_value(
            madvise_choice,
            madvise_choice_available,
            static_cast<std::uint64_t>(choice),
            has_stable_value_conflict);
        record_stable_value(
            madvise_result,
            madvise_result_available,
            static_cast<std::uint64_t>(result),
            has_stable_value_conflict);
    }

    [[nodiscard]] constexpr bool
    provider_identity_complete_and_consistent() const noexcept {
        return provider_configured_available
            && provider_captured_available
            && provider_persisted_available
            && provider_opened_available
            && !has_stable_value_conflict
            && provider_configured == provider_captured
            && provider_configured == provider_persisted
            && provider_configured == provider_opened;
    }

    /// Merge one operation-local provider/lifecycle delta.
    [[nodiscard]] bool merge(
            const grouped_soa_provider_stats& rhs,
            bool& has_identity_conflict) noexcept {
        bool overflow = is_saturated || rhs.is_saturated;
        bool conflict = has_stable_value_conflict || rhs.has_stable_value_conflict;
        merge_stable_value(
            provider_configured, provider_configured_available,
            rhs.provider_configured, rhs.provider_configured_available,
            conflict);
        merge_stable_value(
            provider_captured, provider_captured_available,
            rhs.provider_captured, rhs.provider_captured_available,
            conflict);
        merge_stable_value(
            provider_persisted, provider_persisted_available,
            rhs.provider_persisted, rhs.provider_persisted_available,
            conflict);
        merge_stable_value(
            provider_opened, provider_opened_available,
            rhs.provider_opened, rhs.provider_opened_available,
            conflict);
        merge_stable_value(
            mapped_sink_preallocation_result,
            mapped_sink_preallocation_result_available,
            rhs.mapped_sink_preallocation_result,
            rhs.mapped_sink_preallocation_result_available,
            conflict);
        merge_stable_value(
            lazy_state_layout, lazy_state_layout_available,
            rhs.lazy_state_layout, rhs.lazy_state_layout_available,
            conflict);
        merge_stable_value(
            madvise_choice, madvise_choice_available,
            rhs.madvise_choice, rhs.madvise_choice_available,
            conflict);
        merge_stable_value(
            madvise_result, madvise_result_available,
            rhs.madvise_result, rhs.madvise_result_available,
            conflict);

        const auto add = [&overflow](
                std::uint64_t& destination,
                std::uint64_t increment) noexcept {
            overflow = merge_counter(destination, increment) || overflow;
        };
        add(provider_head_scans, rhs.provider_head_scans);
        add(provider_head_rows, rhs.provider_head_rows);
        add(provider_descriptor_scans, rhs.provider_descriptor_scans);
        add(provider_descriptor_rows, rhs.provider_descriptor_rows);
        add(payload_tuples_formed, rhs.payload_tuples_formed);
        add(payload_tuples_inserted, rhs.payload_tuples_inserted);
        add(payload_tuples_reread, rhs.payload_tuples_reread);
        add(mapped_sink_calls, rhs.mapped_sink_calls);
        add(mapped_sink_bytes, rhs.mapped_sink_bytes);
        add(mapped_sink_flushes, rhs.mapped_sink_flushes);
        add(mapped_sink_coalescer_capacity_bytes,
            rhs.mapped_sink_coalescer_capacity_bytes);
        add(mapped_sink_pwrite_calls, rhs.mapped_sink_pwrite_calls);
        add(mapped_sink_short_writes, rhs.mapped_sink_short_writes);
        add(mapped_sink_eintr_retries, rhs.mapped_sink_eintr_retries);
        add(mapped_sink_preallocation_bytes,
            rhs.mapped_sink_preallocation_bytes);
        add(header_crc_bytes, rhs.header_crc_bytes);
        add(directory_crc_bytes, rhs.directory_crc_bytes);
        add(manifest_crc_bytes, rhs.manifest_crc_bytes);
        add(seal_aggregate_payload_crc_bytes,
            rhs.seal_aggregate_payload_crc_bytes);
        add(open_aggregate_payload_crc_bytes,
            rhs.open_aggregate_payload_crc_bytes);
        add(lazy_extent_cas_winners, rhs.lazy_extent_cas_winners);
        add(lazy_extent_waiters, rhs.lazy_extent_waiters);
        add(lazy_extent_executions, rhs.lazy_extent_executions);
        add(lazy_extent_reuses, rhs.lazy_extent_reuses);
        add(lazy_extent_crc_bytes, rhs.lazy_extent_crc_bytes);
        add(lazy_extent_state_bytes, rhs.lazy_extent_state_bytes);
        add(lazy_extent_is_invalid_receipts,
            rhs.lazy_extent_is_invalid_receipts);
        add(mapping_bytes, rhs.mapping_bytes);
        add(mapping_count, rhs.mapping_count);
        add(mapping_resourceowner_releases,
            rhs.mapping_resourceowner_releases);
        const auto bounded_file_action_high_water =
            std::min(rhs.file_action_high_water, k_sql_bigint_value_max);
        overflow = overflow || rhs.file_action_high_water > k_sql_bigint_value_max;
        file_action_high_water = std::max(
            file_action_high_water, bounded_file_action_high_water);
        add(file_action_identity_mismatches,
            rhs.file_action_identity_mismatches);
        add(reaper_descriptors_examined,
            rhs.reaper_descriptors_examined);
        add(reaper_relation_rows_deleted,
            rhs.reaper_relation_rows_deleted);
        add(reaper_payload_bytes_deleted,
            rhs.reaper_payload_bytes_deleted);
        add(reaper_files_queued, rhs.reaper_files_queued);
        add(reaper_remaining, rhs.reaper_remaining);
        add(orphan_entries_examined, rhs.orphan_entries_examined);
        add(orphan_files_acted, rhs.orphan_files_acted);
        add(orphan_bytes_acted, rhs.orphan_bytes_acted);
        add(orphan_remaining, rhs.orphan_remaining);
        add(orphan_cursor_wraps, rhs.orphan_cursor_wraps);

        is_saturated = overflow;
        has_stable_value_conflict = conflict;
        has_identity_conflict = has_identity_conflict || conflict;
        return overflow;
    }

private:
    static void record_stable_value(
            std::uint64_t& destination,
            bool& destination_available,
            std::uint64_t incoming,
            bool& conflict) noexcept {
        if (!destination_available) {
            destination = incoming;
            destination_available = true;
            return;
        }
        if (destination != incoming) conflict = true;
    }

    static void merge_stable_value(
            std::uint64_t& destination,
            bool& destination_available,
            std::uint64_t incoming,
            bool incoming_available,
            bool& conflict) noexcept {
        if ((!destination_available && destination != 0u)
            || (!incoming_available && incoming != 0u)) {
            conflict = true;
        }
        if (!incoming_available) return;
        record_stable_value(
            destination,
            destination_available,
            incoming,
            conflict);
    }
};

/// Once-per-buffer operational counters and report-once identities.
struct pipeline_operational_stats {
    std::array<pipeline_stage_timing, k_pipeline_stage_count> timing{};
    std::uint64_t gpu_runtime_init_ns = 0;
    std::uint64_t gpu_pipeline_cache_loads = 0;
    std::uint64_t gpu_pipeline_cache_seed_bytes = 0;
    std::uint64_t gpu_pipeline_cache_load_ns = 0;
    std::uint64_t gpu_pipeline_creation_ns = 0;
    std::uint64_t gpu_submit_cpu_ns = 0;
    std::uint64_t gpu_wait_cpu_ns = 0;
    std::uint64_t gpu_ordinal_readback_cpu_ns = 0;
    std::uint64_t gpu_result_readback_cpu_ns = 0;
    std::uint64_t gpu_device_critical_path_ns = 0;
    std::uint64_t gpu_timestamp_query_failures = 0;
    std::uint64_t target_rows = 0;
    std::uint64_t cpu_prepared_index_builds = 0;
    std::uint64_t cpu_prepared_index_reuses = 0;
    std::uint64_t cpu_prepared_unique_keys = 0;
    std::uint64_t cpu_prepared_duplicate_build_attempts = 0;
    // Arbitrary u64 CRC identities, not additive SQL bigint counters. Future
    // serializers emit NULL when *_available is false and lossless u64 text when
    // true (CRC zero is valid and is distinguished by the availability bit).
    std::uint64_t cpu_prepared_key_set_checksum = 0;
    bool cpu_prepared_key_set_checksum_available = false;
    std::uint64_t cpu_prepared_build_set_checksum = 0;
    bool cpu_prepared_build_set_checksum_available = false;
    std::uint64_t persisted_index_rows = 0;
    std::uint64_t missing_persisted_index_rows = 0;
    std::uint64_t recomputed_target_index_rows = 0;
    std::uint64_t persisted_generation_read_sessions = 0;
    std::uint64_t persisted_relation_opens = 0;
    std::uint64_t persisted_index_scans = 0;
    std::uint64_t persisted_head_scans = 0;
    std::uint64_t persisted_head_rows_read = 0;
    std::uint64_t persisted_head_bytes_read = 0;
    std::uint64_t persisted_manifest_directory_scans = 0;
    std::uint64_t persisted_manifest_rows_read = 0;
    std::uint64_t persisted_manifest_bytes_read = 0;
    std::uint64_t persisted_directory_chunks_read = 0;
    std::uint64_t persisted_directory_bytes_read = 0;
    std::uint64_t persisted_metadata_repeated_reads = 0;
    std::uint64_t persisted_metadata_heap_rows_read = 0;
    std::uint64_t persisted_metadata_bytes_read = 0;
    std::uint64_t persisted_payload_heap_rows_read = 0;
    std::uint64_t persisted_payload_bytes_read = 0;
    std::uint64_t persisted_heap_rows_read = 0;
    std::uint64_t persisted_toast_values_read = 0;
    std::uint64_t persisted_detoast_bytes = 0;
    std::uint64_t persisted_explicit_copy_bytes = 0;
    std::uint64_t persisted_wanted_subkeys = 0;
    std::uint64_t persisted_read_subkeys = 0;
    std::uint64_t persisted_wanted_extents = 0;
    std::uint64_t persisted_wanted_chunks = 0;
    std::uint64_t persisted_skipped_rows = 0;
    std::uint64_t persisted_repeated_chunk_reads = 0;
    std::uint64_t persisted_extents_read = 0;
    std::uint64_t persisted_chunks_read = 0;
    std::uint64_t persisted_bytes_read = 0;
    std::uint64_t persisted_bulk_read_calls = 0;
    std::uint64_t persisted_planned_request_groups = 0;
    std::uint64_t persisted_executed_request_groups = 0;
    std::uint64_t persisted_range_scan_calls = 0;
    std::uint64_t persisted_skip_budget_rows = 0;
    std::uint64_t persisted_hash_build_calls = 0;
    std::uint64_t persisted_assembly_ns = 0;
    std::uint64_t grouped_soa_reader_capacity_accounting_passes = 0;
    std::uint64_t grouped_soa_reader_global_position_storage_preparations = 0;
    std::uint64_t grouped_soa_reader_global_position_storage_allocations = 0;
    std::uint64_t grouped_soa_reader_position_append_growth_reservations = 0;
    std::uint64_t grouped_soa_reader_retained_position_count = 0;
    std::uint64_t grouped_soa_reader_retained_position_capacity = 0;
    std::uint64_t grouped_soa_reader_directory_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_query_hash_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_query_record_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_ordinal_route_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_selected_extent_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_current_selected_control_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_global_position_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_control_payload_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_owned_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_admitted_bytes = 0;
    std::uint64_t grouped_soa_reader_exact_position_image_checks = 0;
    std::uint64_t grouped_soa_reader_exact_position_image_failures = 0;
    std::uint64_t grouped_soa_reader_retained_layout_checks = 0;
    std::uint64_t grouped_soa_reader_retained_layout_failures = 0;
    bool are_grouped_soa_reader_memory_stats_saturated = false;
    grouped_soa_provider_stats grouped_soa_provider{};
    bioseq::indexing::prepared_fragment_anchor_batch_stats
        fragment_native_query_stats{};
    hash_major_index_reader_stats hash_major_index_reader{};
    // Continuation-kernel vector batches and resident vector_stream calls are
    // distinct execution units. Keep both truthful; callers that only need an
    // authenticity witness use has_nthash_vector_activity() instead of
    // relabelling one resident kernel call as one continuation batch.
    std::uint64_t nthash_vector_batches = 0;
    std::uint64_t nthash_resident_vector_stream_kernel_calls = 0;
    std::uint64_t nthash_scalar_backend_branches = 0;
    std::uint64_t nthash_scalar_tail_symbols = 0;
    std::uint64_t nthash_masked_tail_lanes = 0;
    std::uint64_t simd_chain_vector_batches = 0;
    std::uint64_t simd_chain_backtrack_vector_batches = 0;
    std::uint64_t simd_chain_scalar_backend_branches = 0;
    std::uint64_t simd_chain_backtrack_scalar_branches = 0;
    std::uint64_t simd_chaining_exact_policy_calls = 0;
    std::uint64_t simd_chaining_bounded_policy_calls = 0;
    std::uint64_t simd_chaining_bounded_score_batches = 0;
    std::uint64_t simd_chaining_bounded_cutoff_stops = 0;
    std::uint64_t simd_chaining_bounded_candidates_skipped = 0;
    std::uint64_t simd_chaining_policy_rejections = 0;
    bool are_simd_chaining_policy_stats_saturated = false;
    block_aligner_stage_stats block_aligner_stats{};
    ::bioseq::prefilter::prefilter_execution_stats
        prefilter_stats{};
    bool prefilter_execution_stats_available = false;
    std::uint64_t cpu_seed_positions_rejected_outside_collection = 0;
    std::uint64_t gpu_partition_select_dispatches = 0;
    std::uint64_t gpu_partition_anchor_merge_dispatches = 0;
    std::uint64_t gpu_index_build_candidates = 0;
    std::uint64_t gpu_index_build_canonical_emitted = 0;
    std::uint64_t gpu_index_build_overlap_duplicates_rejected = 0;
    std::uint64_t gpu_index_build_noncanonical_extras_rejected = 0;
    std::uint64_t gpu_index_build_surviving_extras = 0;
    std::uint64_t gpu_persisted_resident_uploads = 0;
    std::uint64_t gpu_persisted_resident_cache_hits = 0;
    std::uint64_t gpu_overlap_anchor_duplicates = 0;
    std::uint64_t gpu_seed_dispatches = 0;
    std::uint64_t gpu_chain_dispatches = 0;
    std::uint64_t gpu_chain_backtrack_dispatches = 0;
    // Distinct algorithm identity: coordinate vote is not tiled-DP chaining
    // or predecessor backtracking and must never satisfy either stats counter.
    std::uint64_t gpu_coordinate_vote_dispatches = 0;
    std::uint64_t gpu_extend_dispatches = 0;
    std::uint64_t gpu_target_window_request_dispatches = 0;
    std::uint64_t gpu_target_window_logical_units_requested = 0;
    std::uint64_t host_target_window_requests_consumed = 0;
    std::uint64_t host_target_window_physical_units_read = 0;
    std::uint64_t host_target_window_remap_units_filled = 0;
    std::uint64_t host_target_window_packed_bytes_read = 0;
    std::uint64_t gpu_queue_submits = 0;
    std::uint64_t gpu_host_waits = 0;
    std::uint64_t gpu_buffer_creates = 0;
    std::uint64_t gpu_memory_allocations = 0;
    std::uint64_t gpu_pipeline_creates = 0;
    std::uint64_t host_pair_materialization_calls = 0;
    std::uint64_t host_biological_hash_filter_calls = 0;
    std::uint64_t host_query_hash_readbacks = 0;
    std::uint64_t host_query_symbol_packing_calls = 0;
    std::uint64_t host_target_symbol_packing_calls = 0;
    std::uint64_t host_alignment_job_classification_calls = 0;
    std::uint64_t host_anchor_concatenation_calls = 0;
    std::uint64_t host_cigar_construction_calls = 0;
    std::uint64_t pair_sort_calls = 0;
    std::uint64_t full_segment_hydrations = 0;
    std::uint64_t backend_declines = 0;
    std::uint64_t backend_fallbacks = 0;
    // Cold grouped-SoA source/route stats. The conversion counters name only
    // pair-index transformation work while segment-local direct-reader
    // validation is tracked independently.
    std::uint64_t persisted_source_parent_rows_read = 0;
    std::uint64_t persisted_source_catalog_rows_read = 0;
    std::uint64_t persisted_source_catalog_index_scans = 0;
    std::uint64_t persisted_source_fragment_rows_read = 0;
    std::uint64_t persisted_source_decoded_bp = 0;
    std::uint64_t persisted_pair_index_conversion_calls = 0;
    std::uint64_t persisted_segment_rebase_calls = 0;
    // Runtime-only stats recording that the selected fixed-target grouped-SoA
    // validator executes. Plan construction cannot know the persisted wire
    // format selected for a row, so this counter is populated only from the
    // completed prepared grouped-SoA query-reader route stats.
    std::uint64_t indexing_grouped_soa_validate_calls = 0;
    // PostgreSQL cold persisted-index preparation. These are execution
    // diagnostics only; backend/fallback acceptance remains owned by the
    // existing requested/actual stage stats above.
    std::uint64_t cold_index_target_relation_reuses = 0;
    std::uint64_t cold_index_target_relation_extra_opens = 0;
    std::uint64_t cold_index_parent_random_reads = 0;
    std::uint64_t cold_index_policy_heap_opens = 0;
    std::uint64_t cold_index_policy_index_opens = 0;
    std::uint64_t cold_index_source_heap_opens = 0;
    std::uint64_t cold_index_source_index_opens = 0;
    std::uint64_t cold_index_fragment_heap_opens = 0;
    std::uint64_t cold_index_sidecar_heap_opens = 0;
    std::uint64_t cold_index_sidecar_index_opens = 0;
    std::uint64_t cold_index_policy_schema_validations = 0;
    std::uint64_t cold_index_source_schema_validations = 0;
    std::uint64_t cold_index_sidecar_schema_validations = 0;
    std::uint64_t cold_index_head_scans = 0;
    std::uint64_t cold_index_directory_scans = 0;
    std::uint64_t cold_index_payload_range_scans = 0;
    std::uint64_t cold_index_lock_acquires = 0;
    std::uint64_t cold_index_lock_releases = 0;
    std::uint64_t cold_index_snapshot_registers = 0;
    std::uint64_t cold_index_route_empty = 0;
    std::uint64_t cold_index_route_selective = 0;
    std::uint64_t cold_index_route_full = 0;
    std::uint64_t cold_index_route_candidate_rows = 0;
    std::uint64_t cold_index_route_full_rows = 0;
    std::uint64_t cold_index_route_candidate_bytes = 0;
    std::uint64_t cold_index_route_full_bytes = 0;
    std::uint64_t cold_index_route_selective_cost = 0;
    std::uint64_t cold_index_route_full_cost = 0;
    std::uint64_t cold_index_memory_reroutes = 0;
    std::uint64_t cold_index_payload_bytes_requested = 0;
    std::uint64_t cold_index_payload_bytes_delivered = 0;
    std::uint64_t cold_index_payload_rows_requested = 0;
    std::uint64_t cold_index_payload_rows_delivered = 0;
    std::uint64_t cold_index_payload_rows_scanned = 0;
    std::uint64_t cold_index_payload_bytes_scanned = 0;
    std::uint64_t cold_index_payload_rows_skipped = 0;
    std::uint64_t cold_index_heap_tid_adjacent = 0;
    std::uint64_t cold_index_heap_tid_forward_gap = 0;
    std::uint64_t cold_index_heap_tid_backward_jump = 0;
    std::uint64_t cold_index_extent_crc_bytes = 0;
    std::uint64_t cold_index_extent_crc_raw_bytes_hashed = 0;
    std::uint64_t cold_index_extent_crc_combined_bytes = 0;
    std::uint64_t cold_index_extent_crc_combine_rows = 0;
    std::uint64_t cold_index_row_crc_bytes = 0;
    std::uint64_t cold_index_tuple_form_bytes = 0;
    std::uint64_t cold_index_toast_values = 0;
    std::uint64_t cold_index_detoast_bytes = 0;
    std::uint64_t cold_index_policy_ns = 0;
    std::uint64_t cold_index_source_ns = 0;
    std::uint64_t cold_index_bind_ns = 0;
    std::uint64_t cold_index_head_ns = 0;
    std::uint64_t cold_index_directory_ns = 0;
    std::uint64_t cold_index_payload_ns = 0;
    std::uint64_t cold_index_decode_ns = 0;
    std::uint64_t cold_index_transfer_ns = 0;
    std::uint64_t cold_index_close_ns = 0;
    std::uint64_t cold_index_shared_blks_hit = 0;
    std::uint64_t cold_index_shared_blks_read = 0;
    // Dedicated fail-closed identity signal; independent from numeric
    // saturation because arbitrary u64 identities are never clamped.
    bool has_operational_identity_conflict = false;
    bool are_stats_saturated = false;

    // Defaulted comparison is deliberately member-wise. It includes every
    // timing, counter, identity value/presence bit, and failure flag without
    // observing object padding (memcmp is forbidden for stats values).
    [[nodiscard]] constexpr bool operator==(
        const pipeline_operational_stats&) const noexcept = default;

    /// Authentic vector activity may originate from the continuation kernel
    /// or the whole-range resident vector_stream kernel. The two counts remain
    /// separately interpretable above.
    [[nodiscard]] constexpr bool has_nthash_vector_activity() const noexcept {
        return nthash_vector_batches != 0
            || nthash_resident_vector_stream_kernel_calls != 0;
    }

    /// Merge one completed operation-local prefilter stats. The authoritative
    /// stats type owns all counter arithmetic and invariants. The statement
    /// aggregate only retains its presence and propagates invalidity into the
    /// existing fail-closed operational saturation signal.
    void record_prefilter_execution_stats(
            const ::bioseq::prefilter::prefilter_execution_stats&
                stats) noexcept {
        prefilter_stats.merge(stats);
        prefilter_execution_stats_available = true;
        if (!prefilter_stats.is_internally_consistent()) {
            prefilter_stats.is_saturated = true;
            are_stats_saturated = true;
        }
    }

    /// Merge one operational delta without multiplying it by logical jobs.
    void merge(const pipeline_operational_stats& rhs) noexcept {
        using prefilter_stats_type =
            ::bioseq::prefilter::prefilter_execution_stats;
        const prefilter_stats_type empty_prefilter_stats{};
        if (prefilter_execution_stats_available
            && !prefilter_stats.is_internally_consistent()) {
            prefilter_stats.is_saturated = true;
            are_stats_saturated = true;
        } else if (!prefilter_execution_stats_available
                   && prefilter_stats != empty_prefilter_stats) {
            prefilter_stats.is_saturated = true;
            prefilter_execution_stats_available = true;
            are_stats_saturated = true;
        }
        if (rhs.prefilter_execution_stats_available) {
            record_prefilter_execution_stats(
                rhs.prefilter_stats);
        } else if (rhs.prefilter_stats
                   != empty_prefilter_stats) {
            prefilter_stats_type malformed = rhs.prefilter_stats;
            malformed.is_saturated = true;
            record_prefilter_execution_stats(malformed);
        }
        bool is_saturated = are_stats_saturated || rhs.are_stats_saturated;
        has_operational_identity_conflict = has_operational_identity_conflict
            || rhs.has_operational_identity_conflict;
        merge_report_once_identity(
            cpu_prepared_key_set_checksum,
            cpu_prepared_key_set_checksum_available,
            rhs.cpu_prepared_key_set_checksum,
            rhs.cpu_prepared_key_set_checksum_available,
            has_operational_identity_conflict);
        merge_report_once_identity(
            cpu_prepared_build_set_checksum,
            cpu_prepared_build_set_checksum_available,
            rhs.cpu_prepared_build_set_checksum,
            rhs.cpu_prepared_build_set_checksum_available,
            has_operational_identity_conflict);
        auto add = [&is_saturated](
            std::uint64_t& destination,
            std::uint64_t increment) noexcept {
            is_saturated = merge_counter(destination, increment) || is_saturated;
        };
        for (std::size_t i = 0; i < timing.size(); ++i) {
            add(timing[i].host_wall_ns, rhs.timing[i].host_wall_ns);
            add(timing[i].gpu_device_ns, rhs.timing[i].gpu_device_ns);
        }
        add(gpu_runtime_init_ns, rhs.gpu_runtime_init_ns);
        add(gpu_pipeline_cache_loads, rhs.gpu_pipeline_cache_loads);
        add(gpu_pipeline_cache_seed_bytes, rhs.gpu_pipeline_cache_seed_bytes);
        add(gpu_pipeline_cache_load_ns, rhs.gpu_pipeline_cache_load_ns);
        add(gpu_pipeline_creation_ns, rhs.gpu_pipeline_creation_ns);
        add(gpu_submit_cpu_ns, rhs.gpu_submit_cpu_ns);
        add(gpu_wait_cpu_ns, rhs.gpu_wait_cpu_ns);
        add(gpu_ordinal_readback_cpu_ns, rhs.gpu_ordinal_readback_cpu_ns);
        add(gpu_result_readback_cpu_ns, rhs.gpu_result_readback_cpu_ns);
        add(gpu_device_critical_path_ns, rhs.gpu_device_critical_path_ns);
        add(gpu_timestamp_query_failures, rhs.gpu_timestamp_query_failures);
        add(target_rows, rhs.target_rows);
        add(cpu_prepared_index_builds, rhs.cpu_prepared_index_builds);
        add(cpu_prepared_index_reuses, rhs.cpu_prepared_index_reuses);
        add(cpu_prepared_unique_keys, rhs.cpu_prepared_unique_keys);
        add(cpu_prepared_duplicate_build_attempts,
            rhs.cpu_prepared_duplicate_build_attempts);
        add(persisted_index_rows, rhs.persisted_index_rows);
        add(missing_persisted_index_rows, rhs.missing_persisted_index_rows);
        add(recomputed_target_index_rows, rhs.recomputed_target_index_rows);
        add(persisted_generation_read_sessions,
            rhs.persisted_generation_read_sessions);
        add(persisted_relation_opens, rhs.persisted_relation_opens);
        add(persisted_index_scans, rhs.persisted_index_scans);
        add(persisted_head_scans, rhs.persisted_head_scans);
        add(persisted_head_rows_read, rhs.persisted_head_rows_read);
        add(persisted_head_bytes_read, rhs.persisted_head_bytes_read);
        add(persisted_manifest_directory_scans,
            rhs.persisted_manifest_directory_scans);
        add(persisted_manifest_rows_read, rhs.persisted_manifest_rows_read);
        add(persisted_manifest_bytes_read, rhs.persisted_manifest_bytes_read);
        add(persisted_directory_chunks_read, rhs.persisted_directory_chunks_read);
        add(persisted_directory_bytes_read, rhs.persisted_directory_bytes_read);
        add(persisted_metadata_repeated_reads,
            rhs.persisted_metadata_repeated_reads);
        add(persisted_metadata_heap_rows_read,
            rhs.persisted_metadata_heap_rows_read);
        add(persisted_metadata_bytes_read, rhs.persisted_metadata_bytes_read);
        add(persisted_payload_heap_rows_read,
            rhs.persisted_payload_heap_rows_read);
        add(persisted_payload_bytes_read, rhs.persisted_payload_bytes_read);
        add(persisted_heap_rows_read, rhs.persisted_heap_rows_read);
        add(persisted_toast_values_read, rhs.persisted_toast_values_read);
        add(persisted_detoast_bytes, rhs.persisted_detoast_bytes);
        add(persisted_explicit_copy_bytes, rhs.persisted_explicit_copy_bytes);
        add(persisted_wanted_subkeys, rhs.persisted_wanted_subkeys);
        add(persisted_read_subkeys, rhs.persisted_read_subkeys);
        add(persisted_wanted_extents, rhs.persisted_wanted_extents);
        add(persisted_wanted_chunks, rhs.persisted_wanted_chunks);
        add(persisted_skipped_rows, rhs.persisted_skipped_rows);
        add(persisted_repeated_chunk_reads, rhs.persisted_repeated_chunk_reads);
        add(persisted_extents_read, rhs.persisted_extents_read);
        add(persisted_chunks_read, rhs.persisted_chunks_read);
        add(persisted_bytes_read, rhs.persisted_bytes_read);
        add(persisted_bulk_read_calls, rhs.persisted_bulk_read_calls);
        add(persisted_planned_request_groups,
            rhs.persisted_planned_request_groups);
        add(persisted_executed_request_groups,
            rhs.persisted_executed_request_groups);
        add(persisted_range_scan_calls, rhs.persisted_range_scan_calls);
        add(persisted_skip_budget_rows, rhs.persisted_skip_budget_rows);
        add(persisted_hash_build_calls, rhs.persisted_hash_build_calls);
        add(persisted_assembly_ns, rhs.persisted_assembly_ns);
        add(grouped_soa_reader_capacity_accounting_passes,
            rhs.grouped_soa_reader_capacity_accounting_passes);
        add(grouped_soa_reader_global_position_storage_preparations,
            rhs.grouped_soa_reader_global_position_storage_preparations);
        add(grouped_soa_reader_global_position_storage_allocations,
            rhs.grouped_soa_reader_global_position_storage_allocations);
        add(grouped_soa_reader_position_append_growth_reservations,
            rhs.grouped_soa_reader_position_append_growth_reservations);
        add(grouped_soa_reader_retained_position_count,
            rhs.grouped_soa_reader_retained_position_count);
        add(grouped_soa_reader_retained_position_capacity,
            rhs.grouped_soa_reader_retained_position_capacity);
        add(grouped_soa_reader_directory_capacity_bytes,
            rhs.grouped_soa_reader_directory_capacity_bytes);
        add(grouped_soa_reader_query_hash_capacity_bytes,
            rhs.grouped_soa_reader_query_hash_capacity_bytes);
        add(grouped_soa_reader_query_record_capacity_bytes,
            rhs.grouped_soa_reader_query_record_capacity_bytes);
        add(grouped_soa_reader_ordinal_route_capacity_bytes,
            rhs.grouped_soa_reader_ordinal_route_capacity_bytes);
        add(grouped_soa_reader_selected_extent_capacity_bytes,
            rhs.grouped_soa_reader_selected_extent_capacity_bytes);
        add(grouped_soa_reader_current_selected_control_capacity_bytes,
            rhs.grouped_soa_reader_current_selected_control_capacity_bytes);
        add(grouped_soa_reader_global_position_capacity_bytes,
            rhs.grouped_soa_reader_global_position_capacity_bytes);
        add(grouped_soa_reader_control_payload_capacity_bytes,
            rhs.grouped_soa_reader_control_payload_capacity_bytes);
        add(grouped_soa_reader_owned_capacity_bytes,
            rhs.grouped_soa_reader_owned_capacity_bytes);
        add(grouped_soa_reader_admitted_bytes,
            rhs.grouped_soa_reader_admitted_bytes);
        add(grouped_soa_reader_exact_position_image_checks,
            rhs.grouped_soa_reader_exact_position_image_checks);
        add(grouped_soa_reader_exact_position_image_failures,
            rhs.grouped_soa_reader_exact_position_image_failures);
        add(grouped_soa_reader_retained_layout_checks,
            rhs.grouped_soa_reader_retained_layout_checks);
        add(grouped_soa_reader_retained_layout_failures,
            rhs.grouped_soa_reader_retained_layout_failures);
        are_grouped_soa_reader_memory_stats_saturated =
            are_grouped_soa_reader_memory_stats_saturated
            || rhs.are_grouped_soa_reader_memory_stats_saturated;
        is_saturated = grouped_soa_provider.merge(
            rhs.grouped_soa_provider,
            has_operational_identity_conflict) || is_saturated;
        are_grouped_soa_reader_memory_stats_saturated =
            are_grouped_soa_reader_memory_stats_saturated
            || grouped_soa_provider.is_saturated;
        fragment_native_query_stats.merge(
            rhs.fragment_native_query_stats);
        is_saturated = fragment_native_query_stats.saturated || is_saturated;
        is_saturated = hash_major_index_reader.merge(
            rhs.hash_major_index_reader,
            has_operational_identity_conflict) || is_saturated;
        add(nthash_vector_batches, rhs.nthash_vector_batches);
        add(nthash_resident_vector_stream_kernel_calls,
            rhs.nthash_resident_vector_stream_kernel_calls);
        add(nthash_scalar_backend_branches, rhs.nthash_scalar_backend_branches);
        add(nthash_scalar_tail_symbols, rhs.nthash_scalar_tail_symbols);
        add(nthash_masked_tail_lanes, rhs.nthash_masked_tail_lanes);
        add(simd_chain_vector_batches, rhs.simd_chain_vector_batches);
        add(simd_chain_backtrack_vector_batches,
            rhs.simd_chain_backtrack_vector_batches);
        add(simd_chain_scalar_backend_branches,
            rhs.simd_chain_scalar_backend_branches);
        add(simd_chain_backtrack_scalar_branches,
            rhs.simd_chain_backtrack_scalar_branches);
        add(simd_chaining_exact_policy_calls,
            rhs.simd_chaining_exact_policy_calls);
        add(simd_chaining_bounded_policy_calls,
            rhs.simd_chaining_bounded_policy_calls);
        add(simd_chaining_bounded_score_batches,
            rhs.simd_chaining_bounded_score_batches);
        add(simd_chaining_bounded_cutoff_stops,
            rhs.simd_chaining_bounded_cutoff_stops);
        add(simd_chaining_bounded_candidates_skipped,
            rhs.simd_chaining_bounded_candidates_skipped);
        add(simd_chaining_policy_rejections,
            rhs.simd_chaining_policy_rejections);
        are_simd_chaining_policy_stats_saturated =
            are_simd_chaining_policy_stats_saturated
            || rhs.are_simd_chaining_policy_stats_saturated;
        is_saturated = block_aligner_stats.merge(
            rhs.block_aligner_stats) || is_saturated;
        add(cpu_seed_positions_rejected_outside_collection,
            rhs.cpu_seed_positions_rejected_outside_collection);
        add(gpu_partition_select_dispatches,
            rhs.gpu_partition_select_dispatches);
        add(gpu_partition_anchor_merge_dispatches,
            rhs.gpu_partition_anchor_merge_dispatches);
        add(gpu_index_build_candidates, rhs.gpu_index_build_candidates);
        add(gpu_index_build_canonical_emitted,
            rhs.gpu_index_build_canonical_emitted);
        add(gpu_index_build_overlap_duplicates_rejected,
            rhs.gpu_index_build_overlap_duplicates_rejected);
        add(gpu_index_build_noncanonical_extras_rejected,
            rhs.gpu_index_build_noncanonical_extras_rejected);
        add(gpu_index_build_surviving_extras,
            rhs.gpu_index_build_surviving_extras);
        add(gpu_persisted_resident_uploads,
            rhs.gpu_persisted_resident_uploads);
        add(gpu_persisted_resident_cache_hits,
            rhs.gpu_persisted_resident_cache_hits);
        add(gpu_overlap_anchor_duplicates, rhs.gpu_overlap_anchor_duplicates);
        add(gpu_seed_dispatches, rhs.gpu_seed_dispatches);
        add(gpu_chain_dispatches, rhs.gpu_chain_dispatches);
        add(gpu_chain_backtrack_dispatches,
            rhs.gpu_chain_backtrack_dispatches);
        add(gpu_coordinate_vote_dispatches,
            rhs.gpu_coordinate_vote_dispatches);
        add(gpu_extend_dispatches, rhs.gpu_extend_dispatches);
        add(gpu_target_window_request_dispatches,
            rhs.gpu_target_window_request_dispatches);
        add(gpu_target_window_logical_units_requested,
            rhs.gpu_target_window_logical_units_requested);
        add(host_target_window_requests_consumed,
            rhs.host_target_window_requests_consumed);
        add(host_target_window_physical_units_read,
            rhs.host_target_window_physical_units_read);
        add(host_target_window_remap_units_filled,
            rhs.host_target_window_remap_units_filled);
        add(host_target_window_packed_bytes_read,
            rhs.host_target_window_packed_bytes_read);
        add(gpu_queue_submits, rhs.gpu_queue_submits);
        add(gpu_host_waits, rhs.gpu_host_waits);
        add(gpu_buffer_creates, rhs.gpu_buffer_creates);
        add(gpu_memory_allocations, rhs.gpu_memory_allocations);
        add(gpu_pipeline_creates, rhs.gpu_pipeline_creates);
        add(host_pair_materialization_calls,
            rhs.host_pair_materialization_calls);
        add(host_biological_hash_filter_calls,
            rhs.host_biological_hash_filter_calls);
        add(host_query_hash_readbacks, rhs.host_query_hash_readbacks);
        add(host_query_symbol_packing_calls,
            rhs.host_query_symbol_packing_calls);
        add(host_target_symbol_packing_calls,
            rhs.host_target_symbol_packing_calls);
        add(host_alignment_job_classification_calls,
            rhs.host_alignment_job_classification_calls);
        add(host_anchor_concatenation_calls,
            rhs.host_anchor_concatenation_calls);
        add(host_cigar_construction_calls,
            rhs.host_cigar_construction_calls);
        add(pair_sort_calls, rhs.pair_sort_calls);
        add(full_segment_hydrations, rhs.full_segment_hydrations);
        add(backend_declines, rhs.backend_declines);
        add(backend_fallbacks, rhs.backend_fallbacks);
		add(persisted_source_parent_rows_read,
			rhs.persisted_source_parent_rows_read);
		add(persisted_source_catalog_rows_read,
			rhs.persisted_source_catalog_rows_read);
		add(persisted_source_catalog_index_scans,
			rhs.persisted_source_catalog_index_scans);
		add(persisted_source_fragment_rows_read,
			rhs.persisted_source_fragment_rows_read);
		add(persisted_source_decoded_bp, rhs.persisted_source_decoded_bp);
		add(persisted_pair_index_conversion_calls,
			rhs.persisted_pair_index_conversion_calls);
		add(persisted_segment_rebase_calls,
			rhs.persisted_segment_rebase_calls);
        add(indexing_grouped_soa_validate_calls,
            rhs.indexing_grouped_soa_validate_calls);
        add(cold_index_target_relation_reuses,
            rhs.cold_index_target_relation_reuses);
        add(cold_index_target_relation_extra_opens,
            rhs.cold_index_target_relation_extra_opens);
        add(cold_index_parent_random_reads,
            rhs.cold_index_parent_random_reads);
        add(cold_index_policy_heap_opens,
            rhs.cold_index_policy_heap_opens);
        add(cold_index_policy_index_opens,
            rhs.cold_index_policy_index_opens);
        add(cold_index_source_heap_opens,
            rhs.cold_index_source_heap_opens);
        add(cold_index_source_index_opens,
            rhs.cold_index_source_index_opens);
        add(cold_index_fragment_heap_opens,
            rhs.cold_index_fragment_heap_opens);
        add(cold_index_sidecar_heap_opens,
            rhs.cold_index_sidecar_heap_opens);
        add(cold_index_sidecar_index_opens,
            rhs.cold_index_sidecar_index_opens);
        add(cold_index_policy_schema_validations,
            rhs.cold_index_policy_schema_validations);
        add(cold_index_source_schema_validations,
            rhs.cold_index_source_schema_validations);
        add(cold_index_sidecar_schema_validations,
            rhs.cold_index_sidecar_schema_validations);
        add(cold_index_head_scans, rhs.cold_index_head_scans);
        add(cold_index_directory_scans, rhs.cold_index_directory_scans);
        add(cold_index_payload_range_scans,
            rhs.cold_index_payload_range_scans);
        add(cold_index_lock_acquires, rhs.cold_index_lock_acquires);
        add(cold_index_lock_releases, rhs.cold_index_lock_releases);
        add(cold_index_snapshot_registers,
            rhs.cold_index_snapshot_registers);
        add(cold_index_route_empty, rhs.cold_index_route_empty);
        add(cold_index_route_selective, rhs.cold_index_route_selective);
        add(cold_index_route_full, rhs.cold_index_route_full);
        add(cold_index_route_candidate_rows,
            rhs.cold_index_route_candidate_rows);
        add(cold_index_route_full_rows, rhs.cold_index_route_full_rows);
        add(cold_index_route_candidate_bytes,
            rhs.cold_index_route_candidate_bytes);
        add(cold_index_route_full_bytes, rhs.cold_index_route_full_bytes);
        add(cold_index_route_selective_cost,
            rhs.cold_index_route_selective_cost);
        add(cold_index_route_full_cost, rhs.cold_index_route_full_cost);
        add(cold_index_memory_reroutes,
            rhs.cold_index_memory_reroutes);
        add(cold_index_payload_bytes_requested,
            rhs.cold_index_payload_bytes_requested);
        add(cold_index_payload_bytes_delivered,
            rhs.cold_index_payload_bytes_delivered);
        add(cold_index_payload_rows_requested,
            rhs.cold_index_payload_rows_requested);
        add(cold_index_payload_rows_delivered,
            rhs.cold_index_payload_rows_delivered);
        add(cold_index_payload_rows_scanned,
            rhs.cold_index_payload_rows_scanned);
        add(cold_index_payload_bytes_scanned,
            rhs.cold_index_payload_bytes_scanned);
        add(cold_index_payload_rows_skipped,
            rhs.cold_index_payload_rows_skipped);
        add(cold_index_heap_tid_adjacent,
            rhs.cold_index_heap_tid_adjacent);
        add(cold_index_heap_tid_forward_gap,
            rhs.cold_index_heap_tid_forward_gap);
        add(cold_index_heap_tid_backward_jump,
            rhs.cold_index_heap_tid_backward_jump);
        add(cold_index_extent_crc_bytes,
            rhs.cold_index_extent_crc_bytes);
        add(cold_index_extent_crc_raw_bytes_hashed,
            rhs.cold_index_extent_crc_raw_bytes_hashed);
        add(cold_index_extent_crc_combined_bytes,
            rhs.cold_index_extent_crc_combined_bytes);
        add(cold_index_extent_crc_combine_rows,
            rhs.cold_index_extent_crc_combine_rows);
        add(cold_index_row_crc_bytes, rhs.cold_index_row_crc_bytes);
        add(cold_index_tuple_form_bytes,
            rhs.cold_index_tuple_form_bytes);
        add(cold_index_toast_values, rhs.cold_index_toast_values);
        add(cold_index_detoast_bytes, rhs.cold_index_detoast_bytes);
        add(cold_index_policy_ns, rhs.cold_index_policy_ns);
        add(cold_index_source_ns, rhs.cold_index_source_ns);
        add(cold_index_bind_ns, rhs.cold_index_bind_ns);
        add(cold_index_head_ns, rhs.cold_index_head_ns);
        add(cold_index_directory_ns, rhs.cold_index_directory_ns);
        add(cold_index_payload_ns, rhs.cold_index_payload_ns);
        add(cold_index_decode_ns, rhs.cold_index_decode_ns);
        add(cold_index_transfer_ns, rhs.cold_index_transfer_ns);
        add(cold_index_close_ns, rhs.cold_index_close_ns);
        add(cold_index_shared_blks_hit, rhs.cold_index_shared_blks_hit);
        add(cold_index_shared_blks_read,
            rhs.cold_index_shared_blks_read);
        are_stats_saturated = is_saturated;
    }
};

/// Tri-state optional observation plus a fail-closed mixed sentinel.
enum class optional_observation_bool : std::uint8_t {
    not_applicable,
    false_value,
    true_value,
    mixed_invalid,
};

/// Pipeline-cache preparation policy observed by the timed GPU runtime.
enum class pipeline_cache_policy : std::uint8_t {
    not_applicable,
    immutable_preseed_all_hit,
    isolated_miss_diagnostic,
    mixed_invalid,
};

/// Return the stable SQL/report spelling of an optional observation.
[[nodiscard]] constexpr std::string_view optional_observation_name(
    optional_observation_bool value) noexcept {
    switch (value) {
        case optional_observation_bool::not_applicable: return "not_applicable";
        case optional_observation_bool::false_value: return "false";
        case optional_observation_bool::true_value: return "true";
        case optional_observation_bool::mixed_invalid: return "mixed_invalid";
    }
    return "unknown";
}

/// Return the stable SQL/report spelling of a pipeline-cache policy.
[[nodiscard]] constexpr std::string_view pipeline_cache_policy_name(
    pipeline_cache_policy policy) noexcept {
    switch (policy) {
        case pipeline_cache_policy::not_applicable: return "not_applicable";
        case pipeline_cache_policy::immutable_preseed_all_hit:
            return "immutable_preseed_all_hit";
        case pipeline_cache_policy::isolated_miss_diagnostic:
            return "isolated_miss_diagnostic";
        case pipeline_cache_policy::mixed_invalid: return "mixed_invalid";
    }
    return "unknown";
}

/// Return whether every byte/element of a fixed stats field is zero.
template <typename p_value_type, std::size_t p_size>
[[nodiscard]] constexpr bool is_all_zero(
        const std::array<p_value_type, p_size>& value) noexcept {
    for (const p_value_type element : value) {
        if (element != p_value_type{}) return false;
    }
    return true;
}

/// Exact CPU/GPU execution environment captured by the timed runtime.
struct execution_environment_snapshot {
    std::uint32_t effective_cpu_workers = 0;
    std::int64_t highway_compiled_targets = 0;
    std::int64_t highway_cpu_supported_targets = 0;
    std::int64_t highway_runnable_targets = 0;
    std::int64_t highway_executed_target = 0;
    bool is_highway_executed_target_mixed = false;
    bool is_gpu_environment_available = false;
    std::array<std::uint8_t, 16> gpu_device_uuid{};
    std::array<char, 128> gpu_driver_identity{};
    pipeline_cache_policy gpu_pipeline_cache_policy = pipeline_cache_policy::not_applicable;
    std::array<std::uint8_t, 32> gpu_pipeline_cache_seed_sha256{};
    std::uint64_t gpu_pipeline_cache_seed_size = 0;
    std::uint64_t gpu_pipeline_cache_manifest_checksum = 0;
    optional_observation_bool gpu_pipeline_cache_hit = optional_observation_bool::not_applicable;
    bool is_mixed_invalid = false;

    /// True only for the canonical never-captured environment sentinel.
    [[nodiscard]] static constexpr bool is_unset(
        const execution_environment_snapshot& value) noexcept {
        return value.effective_cpu_workers == 0
            && value.highway_compiled_targets == 0
            && value.highway_cpu_supported_targets == 0
            && value.highway_runnable_targets == 0
            && value.highway_executed_target == 0
            && !value.is_highway_executed_target_mixed
            && !value.is_gpu_environment_available
            && is_all_zero(value.gpu_device_uuid)
            && is_all_zero(value.gpu_driver_identity)
            && value.gpu_pipeline_cache_policy
                == pipeline_cache_policy::not_applicable
            && is_all_zero(value.gpu_pipeline_cache_seed_sha256)
            && value.gpu_pipeline_cache_seed_size == 0
            && value.gpu_pipeline_cache_manifest_checksum == 0
            && value.gpu_pipeline_cache_hit
                == optional_observation_bool::not_applicable
            && !value.is_mixed_invalid;
    }

    /// Compare every environment identity field except mixed-invalid state.
    [[nodiscard]] static constexpr bool is_payload_equal(
        const execution_environment_snapshot& lhs,
        const execution_environment_snapshot& rhs) noexcept {
        return non_highway_is_payload_equal(lhs, rhs)
            && lhs.highway_compiled_targets
                == rhs.highway_compiled_targets
            && lhs.highway_cpu_supported_targets
                == rhs.highway_cpu_supported_targets
            && lhs.highway_runnable_targets
                == rhs.highway_runnable_targets;
    }

    /// Compare environment identity while treating a zero Highway mask as an
    /// observation that has not arrived yet. This identity predicate is used
    /// only while merging independently completed stats shards;
    /// final validation still requires an all-or-none, internally consistent
    /// Highway plan and a singular executed target.
    [[nodiscard]] static constexpr bool has_same_payload_identity(
        const execution_environment_snapshot& lhs,
        const execution_environment_snapshot& rhs) noexcept {
        const auto mask_compatible = [](const std::int64_t left,
                                        const std::int64_t right) noexcept {
            return left == 0 || right == 0 || left == right;
        };
        return non_highway_is_payload_equal(lhs, rhs)
            && mask_compatible(
                lhs.highway_compiled_targets,
                rhs.highway_compiled_targets)
            && mask_compatible(
                lhs.highway_cpu_supported_targets,
                rhs.highway_cpu_supported_targets)
            && mask_compatible(
                lhs.highway_runnable_targets,
                rhs.highway_runnable_targets);
    }

    /// True when no Highway plan or binding has been attached.  This sentinel
    /// is valid while constructing a non-Highway portion of an environment,
    /// but partially populated Highway stats are never accepted.
    [[nodiscard]] static constexpr bool has_no_highway_fields(
        const execution_environment_snapshot& value) noexcept {
        return value.highway_compiled_targets == 0
            && value.highway_cpu_supported_targets == 0
            && value.highway_runnable_targets == 0
            && value.highway_executed_target == 0
            && !value.is_highway_executed_target_mixed;
    }

    /// Validate the exact plan-mask relation and the binding returned by the
    /// sole dynamic-dispatch root. Once any Highway field is available, absence,
    /// a multi-bit target, or a target outside the runnable set fails closed.
    [[nodiscard]] static constexpr bool has_valid_highway_fields(
        const execution_environment_snapshot& value) noexcept {
        if (has_no_highway_fields(value)) return true;
        if (value.is_highway_executed_target_mixed
            || value.highway_compiled_targets == 0
            || value.highway_cpu_supported_targets == 0
            || value.highway_runnable_targets == 0
            || value.highway_runnable_targets
                != (value.highway_compiled_targets
                    & value.highway_cpu_supported_targets)) {
            return false;
        }
        const auto executed = static_cast<std::uint64_t>(
            value.highway_executed_target);
        const auto runnable = static_cast<std::uint64_t>(
            value.highway_runnable_targets);
        return std::has_single_bit(executed)
            && (executed & runnable) == executed;
    }

private:
    [[nodiscard]] static constexpr bool non_highway_is_payload_equal(
        const execution_environment_snapshot& lhs,
        const execution_environment_snapshot& rhs) noexcept {
        return lhs.effective_cpu_workers == rhs.effective_cpu_workers
            && lhs.is_gpu_environment_available == rhs.is_gpu_environment_available
            && lhs.gpu_device_uuid == rhs.gpu_device_uuid
            && lhs.gpu_driver_identity == rhs.gpu_driver_identity
            && lhs.gpu_pipeline_cache_policy == rhs.gpu_pipeline_cache_policy
            && lhs.gpu_pipeline_cache_seed_sha256
                == rhs.gpu_pipeline_cache_seed_sha256
            && lhs.gpu_pipeline_cache_seed_size
                == rhs.gpu_pipeline_cache_seed_size
            && lhs.gpu_pipeline_cache_manifest_checksum
                == rhs.gpu_pipeline_cache_manifest_checksum
            && lhs.gpu_pipeline_cache_hit == rhs.gpu_pipeline_cache_hit;
    }

public:

    /// Adopt the first complete stats and fail closed on absence or drift.
    void merge(const execution_environment_snapshot& rhs) noexcept {
        if (is_unset(*this)) {
            *this = rhs;
            if (rhs.effective_cpu_workers == 0 || rhs.is_mixed_invalid) {
                is_mixed_invalid = true;
            }
            return;
        }
        if (is_unset(rhs)
            || effective_cpu_workers == 0
            || rhs.effective_cpu_workers == 0
            || is_mixed_invalid
            || rhs.is_mixed_invalid
            || !has_same_payload_identity(*this, rhs)) {
            is_mixed_invalid = true;
            return;
        }

        const auto merge_highway_mask = [this](
                std::int64_t& destination,
                const std::int64_t observed) noexcept {
            if (observed == 0) return;
            if (destination == 0) {
                destination = observed;
            } else if (destination != observed) {
                is_mixed_invalid = true;
            }
        };
        merge_highway_mask(
            highway_compiled_targets, rhs.highway_compiled_targets);
        merge_highway_mask(
            highway_cpu_supported_targets,
            rhs.highway_cpu_supported_targets);
        merge_highway_mask(
            highway_runnable_targets, rhs.highway_runnable_targets);

        if (is_highway_executed_target_mixed
            || rhs.is_highway_executed_target_mixed) {
            is_highway_executed_target_mixed = true;
            highway_executed_target = 0;
        } else if (highway_executed_target == 0) {
            highway_executed_target = rhs.highway_executed_target;
        } else if (rhs.highway_executed_target != 0
            && highway_executed_target
                != rhs.highway_executed_target) {
            is_highway_executed_target_mixed = true;
            highway_executed_target = 0;
        }
    }

    /// Record the registry plan and the binding returned by the sole runtime
    /// dispatch. Plan-mask drift is an environment invariant failure. Target
    /// drift is represented explicitly so aggregation never reports a false
    /// singular architecture.
    void record_highway_target(
        const ::bioseq::simd_dispatch::highway_dispatch_plan& plan,
        const std::int64_t executed_target) noexcept
    {
        if (highway_compiled_targets == 0
            && highway_cpu_supported_targets == 0
            && highway_runnable_targets == 0) {
            highway_compiled_targets = plan.compiled_targets;
            highway_cpu_supported_targets = plan.cpu_supported_targets;
            highway_runnable_targets = plan.runnable_targets;
        } else if (highway_compiled_targets != plan.compiled_targets
            || highway_cpu_supported_targets
                != plan.cpu_supported_targets
            || highway_runnable_targets != plan.runnable_targets) {
            is_mixed_invalid = true;
        }

        const auto executed = static_cast<std::uint64_t>(executed_target);
        const auto runnable = static_cast<std::uint64_t>(
            plan.runnable_targets);
        if (plan.compiled_targets == 0
            || plan.cpu_supported_targets == 0
            || plan.runnable_targets == 0
            || plan.runnable_targets
                != (plan.compiled_targets & plan.cpu_supported_targets)
            || plan.planned_target != executed_target
            || !std::has_single_bit(executed)
            || (executed & runnable) != executed) {
            is_mixed_invalid = true;
            return;
        }
        if (is_highway_executed_target_mixed) {
            return;
        }
        if (highway_executed_target == 0) {
            highway_executed_target = executed_target;
        } else if (highway_executed_target != executed_target) {
            highway_executed_target = 0;
            is_highway_executed_target_mixed = true;
        }
    }
};

/// Complete attestation retained until one logical job is validated.
struct pipeline_job_execution_stats {
    executed_backend requested_backend = executed_backend::none;
    executed_backend actual_backend = executed_backend::none;
    simd_chaining_policy_identity requested_simd_chaining_policy =
        simd_chaining_policy_identity::none;
    simd_chaining_policy_identity observed_simd_chaining_policy =
        simd_chaining_policy_identity::none;
    simd_alignment_algorithm_identity requested_simd_alignment_algorithm =
        simd_alignment_algorithm_identity::none;
    simd_alignment_algorithm_identity observed_simd_alignment_algorithm =
        simd_alignment_algorithm_identity::none;
    target_index_source requested_target_index_source = target_index_source::none;
    target_index_source actual_target_index_source = target_index_source::none;
    std::uint64_t persisted_generation_id = 0;
    std::uint64_t persisted_source_identity = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t query_ordinal = 0;
    execution_environment_snapshot environment{};
    pipeline_job_stage_coverage stages{};
};

/// True only for an executable, non-sentinel biological backend.
[[nodiscard]] constexpr bool is_concrete_backend(executed_backend backend) noexcept {
    return backend == executed_backend::scalar_cpu
        || backend == executed_backend::simd_cpu
        || backend == executed_backend::vulkan_gpu;
}

/// True only for a concrete recomputed or persisted index source.
[[nodiscard]] constexpr bool is_concrete_target_index_source(
    target_index_source source) noexcept {
    return source == target_index_source::recomputed
        || source == target_index_source::persisted;
}

/// Merge backend identities. Any concrete disagreement becomes mixed-invalid.
[[nodiscard]] constexpr executed_backend merge_backend_identity(
    executed_backend lhs,
    executed_backend rhs) noexcept {
    if (lhs == executed_backend::none) return rhs;
    if (rhs == executed_backend::none) return lhs;
    if (lhs == rhs) return lhs;
    return executed_backend::mixed_invalid;
}

/// Merge index-source identities. Any concrete disagreement becomes mixed-invalid.
[[nodiscard]] constexpr target_index_source merge_target_index_identity(
    target_index_source lhs,
    target_index_source rhs) noexcept {
    if (lhs == target_index_source::none) return rhs;
    if (rhs == target_index_source::none) return lhs;
    if (lhs == rhs) return lhs;
    return target_index_source::mixed_invalid;
}

/// True only for one concrete, operation-attestable SIMD chaining policy.
[[nodiscard]] constexpr bool is_concrete_simd_chaining_policy_identity(
    simd_chaining_policy_identity policy) noexcept {
    return policy == simd_chaining_policy_identity::exact_visited
        || policy
            == simd_chaining_policy_identity::bounded_batch_approximate;
}

/// Merge policy identities. A concrete disagreement remains visible and
/// revokes aggregate attestation instead of silently choosing either policy.
[[nodiscard]] constexpr simd_chaining_policy_identity
merge_simd_chaining_policy_identity(
    simd_chaining_policy_identity lhs,
    simd_chaining_policy_identity rhs) noexcept {
    if (lhs == simd_chaining_policy_identity::none) return rhs;
    if (rhs == simd_chaining_policy_identity::none) return lhs;
    if (lhs == rhs) return lhs;
    return simd_chaining_policy_identity::mixed_invalid;
}

[[nodiscard]] constexpr bool is_concrete_simd_alignment_algorithm_identity(
    simd_alignment_algorithm_identity algorithm) noexcept {
    return algorithm == simd_alignment_algorithm_identity::block_aligner;
}

[[nodiscard]] constexpr simd_alignment_algorithm_identity
merge_simd_alignment_algorithm_identity(
    simd_alignment_algorithm_identity lhs,
    simd_alignment_algorithm_identity rhs) noexcept {
    if (lhs == simd_alignment_algorithm_identity::none) return rhs;
    if (rhs == simd_alignment_algorithm_identity::none) return lhs;
    if (lhs == rhs) return lhs;
    return simd_alignment_algorithm_identity::mixed_invalid;
}

/// Only an all-SIMD backend may publish SIMD base-alignment identity, and its
/// requested and operation-observed identities must both be Block Aligner.
[[nodiscard]] constexpr bool is_simd_alignment_algorithm_identity_valid_for_backend(
    executed_backend backend,
    simd_alignment_algorithm_identity requested,
    simd_alignment_algorithm_identity observed) noexcept {
    if (backend == executed_backend::simd_cpu) {
        return is_concrete_simd_alignment_algorithm_identity(requested)
            && requested == observed;
    }
    return requested == simd_alignment_algorithm_identity::none
        && observed == simd_alignment_algorithm_identity::none;
}

/// A concrete all-SIMD job must retain the same requested and observed
/// policy. Scalar/GPU jobs must carry no SIMD-policy identity at all.
[[nodiscard]] constexpr bool is_simd_chaining_policy_identity_valid_for_backend(
    executed_backend backend,
    simd_chaining_policy_identity requested,
    simd_chaining_policy_identity observed) noexcept {
    if (backend == executed_backend::simd_cpu) {
        return is_concrete_simd_chaining_policy_identity(requested)
            && requested == observed;
    }
    return requested == simd_chaining_policy_identity::none
        && observed == simd_chaining_policy_identity::none;
}

/// Validate nonempty UTF-8 followed by one NUL and canonical zero padding.
[[nodiscard]] constexpr bool is_canonical_nonempty_driver_identity(
    const std::array<char, 128>& identity) noexcept {
    const auto byte_at = [&identity](std::size_t index) noexcept {
        return static_cast<std::uint8_t>(identity[index]);
    };
    const auto continuation = [&byte_at](std::size_t index) noexcept {
        const std::uint8_t byte = byte_at(index);
        return byte >= 0x80u && byte <= 0xbfu;
    };
    if (byte_at(0) == 0) return false;
    std::size_t index = 0;
    while (index < identity.size()) {
        const std::uint8_t lead = byte_at(index);
        if (lead == 0) {
            for (; index < identity.size(); ++index) {
                if (byte_at(index) != 0) return false;
            }
            return true;
        }
        if (lead <= 0x7fu) {
            ++index;
            continue;
        }
        if (lead >= 0xc2u && lead <= 0xdfu) {
            if (index + 1 >= identity.size() || !continuation(index + 1)) return false;
            index += 2;
            continue;
        }
        if (lead >= 0xe0u && lead <= 0xefu) {
            if (index + 2 >= identity.size()) return false;
            const std::uint8_t second = byte_at(index + 1);
            const bool second_valid = lead == 0xe0u
                ? second >= 0xa0u && second <= 0xbfu
                : lead == 0xedu
                    ? second >= 0x80u && second <= 0x9fu
                    : second >= 0x80u && second <= 0xbfu;
            if (!second_valid || !continuation(index + 2)) return false;
            index += 3;
            continue;
        }
        if (lead >= 0xf0u && lead <= 0xf4u) {
            if (index + 3 >= identity.size()) return false;
            const std::uint8_t second = byte_at(index + 1);
            const bool second_valid = lead == 0xf0u
                ? second >= 0x90u && second <= 0xbfu
                : lead == 0xf4u
                    ? second >= 0x80u && second <= 0x8fu
                    : second >= 0x80u && second <= 0xbfu;
            if (!second_valid
                || !continuation(index + 2)
                || !continuation(index + 3)) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return false;
}

/// Validate CPU/GPU environment presence, identity, and SQL-bound ranges.
[[nodiscard]] constexpr bool is_environment_valid_for_backend(
    const execution_environment_snapshot& environment,
    executed_backend backend) noexcept {
    if (environment.is_mixed_invalid
        || environment.effective_cpu_workers == 0
        || environment.effective_cpu_workers > k_sql_integer_value_max
        || environment.gpu_pipeline_cache_seed_size > k_sql_bigint_value_max
        || !execution_environment_snapshot::has_valid_highway_fields(environment)) {
        return false;
    }
    if (backend == executed_backend::scalar_cpu || backend == executed_backend::simd_cpu) {
        return !environment.is_gpu_environment_available
            && is_all_zero(environment.gpu_device_uuid)
            && is_all_zero(environment.gpu_driver_identity)
            && environment.gpu_pipeline_cache_policy == pipeline_cache_policy::not_applicable
            && is_all_zero(environment.gpu_pipeline_cache_seed_sha256)
            && environment.gpu_pipeline_cache_seed_size == 0
            && environment.gpu_pipeline_cache_manifest_checksum == 0
            && environment.gpu_pipeline_cache_hit == optional_observation_bool::not_applicable;
    }
    if (backend != executed_backend::vulkan_gpu) return false;
    const bool common_gpu_identity_valid =
        environment.is_gpu_environment_available
        && !is_all_zero(environment.gpu_device_uuid)
        && is_canonical_nonempty_driver_identity(environment.gpu_driver_identity)
        && !is_all_zero(environment.gpu_pipeline_cache_seed_sha256)
        && environment.gpu_pipeline_cache_manifest_checksum != 0;
    if (!common_gpu_identity_valid) return false;
    if (environment.gpu_pipeline_cache_policy
            == pipeline_cache_policy::immutable_preseed_all_hit) {
        return environment.gpu_pipeline_cache_seed_size > 0
            && environment.gpu_pipeline_cache_hit
                == optional_observation_bool::true_value;
    }
    if (environment.gpu_pipeline_cache_policy
            == pipeline_cache_policy::isolated_miss_diagnostic) {
        return environment.gpu_pipeline_cache_hit
            == optional_observation_bool::false_value;
    }
    return false;
}

/// Return the sole permitted engine for a stage/mode. execution_engine::count
/// means that the stage must be entirely ineligible in this mode.
[[nodiscard]] constexpr execution_engine expected_execution_engine(
    executed_backend backend,
    target_index_source source,
    pipeline_stage stage) noexcept {
    if (!is_concrete_backend(backend) || !is_concrete_target_index_source(source)) {
        return execution_engine::count;
    }
    const execution_engine backend_engine = backend == executed_backend::vulkan_gpu
        ? execution_engine::vulkan_gpu
        : backend == executed_backend::simd_cpu
            ? execution_engine::simd_cpu
            : execution_engine::scalar_cpu;
    switch (stage) {
        case pipeline_stage::query_sketch:
            return backend_engine;
        case pipeline_stage::target_sketch:
            return source == target_index_source::recomputed
                ? backend_engine
                : execution_engine::count;
        case pipeline_stage::target_index_build:
            if (source != target_index_source::recomputed) return execution_engine::count;
            return backend == executed_backend::vulkan_gpu
                ? execution_engine::vulkan_gpu
                : execution_engine::scalar_cpu;
        case pipeline_stage::persisted_index_decode:
            return source == target_index_source::persisted
                ? execution_engine::host_io
                : execution_engine::count;
        case pipeline_stage::partition_selection:
            if (source != target_index_source::persisted) return execution_engine::count;
            return backend == executed_backend::vulkan_gpu
                ? execution_engine::vulkan_gpu
                : execution_engine::scalar_cpu;
        case pipeline_stage::device_index_upload:
            return source == target_index_source::persisted
                    && backend == executed_backend::vulkan_gpu
                ? execution_engine::vulkan_gpu
                : execution_engine::count;
        case pipeline_stage::seed_lookup:
            return backend == executed_backend::vulkan_gpu
                ? execution_engine::vulkan_gpu
                : execution_engine::scalar_cpu;
        case pipeline_stage::chain_score:
        case pipeline_stage::chain_backtrack:
            return backend_engine;
        case pipeline_stage::region_generation:
        case pipeline_stage::primary_secondary_reduction:
        case pipeline_stage::divergence_estimation:
        case pipeline_stage::mapq:
        case pipeline_stage::target_window_plan:
        case pipeline_stage::cigar_job_generation:
            return backend == executed_backend::vulkan_gpu
                ? execution_engine::vulkan_gpu
                : execution_engine::scalar_cpu;
        case pipeline_stage::target_window_io:
            return execution_engine::host_io;
        case pipeline_stage::target_window_upload:
            return backend == executed_backend::vulkan_gpu
                ? execution_engine::vulkan_gpu
                : execution_engine::count;
        case pipeline_stage::left_extension:
        case pipeline_stage::gap_extension:
        case pipeline_stage::right_extension:
            return backend_engine;
        case pipeline_stage::count:
            return execution_engine::count;
    }
    return execution_engine::count;
}

/// Validate binary per-job counts and exclusive execution-engine attribution.
[[nodiscard]] constexpr bool is_stage_coverage_exclusive(
    const stage_coverage& coverage,
    execution_engine expected_engine) noexcept {
    if (coverage.eligible_jobs > 1 || coverage.fallback_jobs > 1) return false;
    for (const std::uint64_t executed : coverage.executed_jobs) {
        if (executed > 1) return false;
    }
    if (coverage.fallback_jobs != 0) return false;
    if (expected_engine == execution_engine::count) {
        if (coverage.eligible_jobs != 0) return false;
        for (const std::uint64_t executed : coverage.executed_jobs) {
            if (executed != 0) return false;
        }
        return true;
    }
    for (std::size_t i = 0; i < coverage.executed_jobs.size(); ++i) {
        const std::uint64_t expected = i == engine_index(expected_engine)
            ? coverage.eligible_jobs
            : 0;
        if (coverage.executed_jobs[i] != expected) return false;
    }
    return true;
}

/// The unconditional front end must execute once even for a zero-match job.
/// Every later stage is data-dependent and may validly report 0/0 while still
/// retaining binary counts and exclusive engine attribution when it does run.
[[nodiscard]] constexpr bool is_front_end_eligibility_required(
    target_index_source source,
    pipeline_stage stage) noexcept {
    if (stage == pipeline_stage::query_sketch) return true;
    if (source == target_index_source::recomputed) {
        return stage == pipeline_stage::target_sketch
            || stage == pipeline_stage::target_index_build;
    }
    if (source == target_index_source::persisted) {
        return stage == pipeline_stage::persisted_index_decode
            || stage == pipeline_stage::partition_selection;
    }
    return false;
}

/// Fail-closed validator for one complete logical target-query stats.
[[nodiscard]] constexpr bool is_job_execution_stats_valid(
    const pipeline_job_execution_stats& job) noexcept {
    if (!is_concrete_backend(job.requested_backend)
        || job.requested_backend != job.actual_backend
        || !is_simd_chaining_policy_identity_valid_for_backend(
            job.actual_backend,
            job.requested_simd_chaining_policy,
            job.observed_simd_chaining_policy)
        || !is_simd_alignment_algorithm_identity_valid_for_backend(
            job.actual_backend,
            job.requested_simd_alignment_algorithm,
            job.observed_simd_alignment_algorithm)
        || !is_concrete_target_index_source(job.requested_target_index_source)
        || job.requested_target_index_source != job.actual_target_index_source
        || execution_environment_snapshot::has_no_highway_fields(
            job.environment)
        || !is_environment_valid_for_backend(job.environment, job.actual_backend)) {
        return false;
    }
    if (job.logical_target_row > k_sql_bigint_value_max
        || job.query_ordinal > k_sql_bigint_value_max) {
        return false;
    }
    if (job.actual_target_index_source == target_index_source::persisted) {
        if (job.persisted_generation_id == 0
            || job.persisted_generation_id > k_sql_bigint_value_max
            || job.persisted_source_identity == 0) {
            return false;
        }
    } else if (job.persisted_generation_id != 0 || job.persisted_source_identity != 0) {
        return false;
    }
    for (std::size_t i = 0; i < k_pipeline_stage_count; ++i) {
        const auto stage = static_cast<pipeline_stage>(i);
        const execution_engine expected_engine = expected_execution_engine(
            job.actual_backend, job.actual_target_index_source, stage);
        if (!is_stage_coverage_exclusive(
                job.stages.coverage[i],
                expected_engine)) {
            return false;
        }
        if (expected_engine != execution_engine::count
            && is_front_end_eligibility_required(
                job.actual_target_index_source, stage)
            && job.stages.coverage[i].eligible_jobs != 1) {
            return false;
        }
    }
    return true;
}

/// Saturating statement/node aggregate of job and operational stats.
struct pipeline_execution_stats {
    executed_backend requested_backend = executed_backend::none;
    executed_backend actual_backend = executed_backend::none;
    simd_chaining_policy_identity requested_simd_chaining_policy =
        simd_chaining_policy_identity::none;
    simd_chaining_policy_identity observed_simd_chaining_policy =
        simd_chaining_policy_identity::none;
    simd_alignment_algorithm_identity requested_simd_alignment_algorithm =
        simd_alignment_algorithm_identity::none;
    simd_alignment_algorithm_identity observed_simd_alignment_algorithm =
        simd_alignment_algorithm_identity::none;
    target_index_source requested_target_index_source = target_index_source::none;
    target_index_source actual_target_index_source = target_index_source::none;
    std::uint64_t persisted_generation_id = 0;
    std::uint64_t persisted_source_identity = 0;
    bool has_mixed_persisted_identity = false;
    std::uint64_t attempted_alignment_jobs = 0;
    std::uint64_t fully_attested_alignment_jobs = 0;
    std::uint64_t unattested_alignment_jobs = 0;
    // Internal merge provenance. These are intentionally not part of the SQL
    // ABI: they let an aggregate-level identity/environment failure name the
    // first attempted job that must be reclassified.
    std::uint64_t first_attempted_target_row = k_unset_job_ordinal;
    std::uint64_t first_attempted_query_ordinal = k_unset_job_ordinal;
    std::uint64_t first_unattested_target_row = k_unset_job_ordinal;
    std::uint64_t first_unattested_query_ordinal = k_unset_job_ordinal;
    execution_environment_snapshot environment{};
    pipeline_job_stage_coverage stages{};
    pipeline_operational_stats operational{};

    /// Once aggregate arithmetic or a report-once identity becomes lossy, no
    /// subset may continue to appear fully attested. Preserve the first
    /// attempted coordinates so the fail-closed transition remains diagnosable.
    void reclassify_all_attempted_jobs() noexcept {
        if (attempted_alignment_jobs == 0) return;
        fully_attested_alignment_jobs = 0;
        unattested_alignment_jobs = attempted_alignment_jobs;
        // A global downgrade includes every earlier job, so its first failure
        // is necessarily the first attempt, not the later job that exposed the
        // drift or malformed stats.
        first_unattested_target_row = first_attempted_target_row;
        first_unattested_query_ordinal = first_attempted_query_ordinal;
    }

    /// Validate and merge one logical target-query stats exactly once.
    void merge_job(
        const pipeline_job_execution_stats& job) noexcept {
        bool attested = is_job_execution_stats_valid(job);
        if ((requested_backend != executed_backend::none
                && requested_backend != job.requested_backend)
            || (actual_backend != executed_backend::none
                && actual_backend != job.actual_backend)
            || (requested_simd_chaining_policy
                    != simd_chaining_policy_identity::none
                && requested_simd_chaining_policy
                    != job.requested_simd_chaining_policy)
            || (observed_simd_chaining_policy
                    != simd_chaining_policy_identity::none
                && observed_simd_chaining_policy
                    != job.observed_simd_chaining_policy)
            || (requested_simd_alignment_algorithm
                    != simd_alignment_algorithm_identity::none
                && requested_simd_alignment_algorithm
                    != job.requested_simd_alignment_algorithm)
            || (observed_simd_alignment_algorithm
                    != simd_alignment_algorithm_identity::none
                && observed_simd_alignment_algorithm
                    != job.observed_simd_alignment_algorithm)
            || (requested_target_index_source != target_index_source::none
                && requested_target_index_source != job.requested_target_index_source)
            || (actual_target_index_source != target_index_source::none
                && actual_target_index_source != job.actual_target_index_source)
            || (!execution_environment_snapshot::is_unset(environment)
                && (!execution_environment_snapshot::has_same_payload_identity(
                        environment, job.environment)
                    || environment.is_mixed_invalid
                    || job.environment.is_mixed_invalid))) {
            attested = false;
        }

        if (job.actual_target_index_source == target_index_source::persisted) {
            const bool complete_job_identity =
                job.persisted_generation_id != 0 && job.persisted_source_identity != 0;
            if (!complete_job_identity) {
                has_mixed_persisted_identity = true;
                attested = false;
            } else if (persisted_generation_id == 0 && persisted_source_identity == 0) {
                persisted_generation_id = job.persisted_generation_id;
                persisted_source_identity = job.persisted_source_identity;
            } else if (persisted_generation_id != job.persisted_generation_id
                       || persisted_source_identity != job.persisted_source_identity) {
                has_mixed_persisted_identity = true;
                attested = false;
            }
        } else if (job.persisted_generation_id != 0 || job.persisted_source_identity != 0) {
            has_mixed_persisted_identity = true;
            attested = false;
        }

        requested_backend = merge_backend_identity(requested_backend, job.requested_backend);
        actual_backend = merge_backend_identity(actual_backend, job.actual_backend);
        requested_simd_chaining_policy = merge_simd_chaining_policy_identity(
            requested_simd_chaining_policy,
            job.requested_simd_chaining_policy);
        observed_simd_chaining_policy = merge_simd_chaining_policy_identity(
            observed_simd_chaining_policy,
            job.observed_simd_chaining_policy);
        requested_simd_alignment_algorithm =
            merge_simd_alignment_algorithm_identity(
                requested_simd_alignment_algorithm,
                job.requested_simd_alignment_algorithm);
        observed_simd_alignment_algorithm =
            merge_simd_alignment_algorithm_identity(
                observed_simd_alignment_algorithm,
                job.observed_simd_alignment_algorithm);
        requested_target_index_source = merge_target_index_identity(
            requested_target_index_source, job.requested_target_index_source);
        actual_target_index_source = merge_target_index_identity(
            actual_target_index_source, job.actual_target_index_source);
        environment.merge(job.environment);

        if (first_attempted_target_row == k_unset_job_ordinal) {
            first_attempted_target_row = job.logical_target_row;
            first_attempted_query_ordinal = job.query_ordinal;
        }
        bool is_saturated = merge_counter(attempted_alignment_jobs, 1);
        if (attested) {
            is_saturated = merge_counter(fully_attested_alignment_jobs, 1) || is_saturated;
        } else {
            is_saturated = merge_counter(unattested_alignment_jobs, 1) || is_saturated;
            if (first_unattested_target_row == k_unset_job_ordinal) {
                first_unattested_target_row = job.logical_target_row;
                first_unattested_query_ordinal = job.query_ordinal;
            }
        }
        is_saturated = stages.merge(job.stages) || is_saturated;
        operational.are_stats_saturated = operational.are_stats_saturated || is_saturated;
        if (!attested
            || unattested_alignment_jobs != 0
            || operational.are_stats_saturated
            || operational.has_operational_identity_conflict
            || operational.backend_declines != 0
            || operational.backend_fallbacks != 0
            || operational.simd_chaining_policy_rejections != 0
            || operational.are_simd_chaining_policy_stats_saturated) {
            reclassify_all_attempted_jobs();
        }
    }

    /// Merge a once-per-buffer operational delta.
    void merge_operational(
        const pipeline_operational_stats& batch) noexcept {
        operational.merge(batch);
        if (operational.are_stats_saturated
            || operational.has_operational_identity_conflict
            || operational.backend_declines != 0
            || operational.backend_fallbacks != 0
            || operational.simd_chaining_policy_rejections != 0
            || operational.are_simd_chaining_policy_stats_saturated) {
            reclassify_all_attempted_jobs();
        }
    }

    /// Detect actual Vulkan work before interpreting a mixed diagnostic's
    /// environment. Selection labels alone are intentionally insufficient.
    [[nodiscard]] static constexpr bool has_gpu_execution_stats(
        const pipeline_execution_stats& stats) noexcept {
        const auto& operational = stats.operational;
        for (std::size_t i = 0; i < k_pipeline_stage_count; ++i) {
            if (stats.stages.coverage[i].executed_jobs[
                    engine_index(execution_engine::vulkan_gpu)] != 0
                || operational.timing[i].gpu_device_ns != 0) {
                return true;
            }
        }
        return operational.gpu_runtime_init_ns != 0
            || operational.gpu_pipeline_cache_loads != 0
            || operational.gpu_pipeline_cache_seed_bytes != 0
            || operational.gpu_pipeline_cache_load_ns != 0
            || operational.gpu_pipeline_creation_ns != 0
            || operational.gpu_submit_cpu_ns != 0
            || operational.gpu_wait_cpu_ns != 0
            || operational.gpu_ordinal_readback_cpu_ns != 0
            || operational.gpu_result_readback_cpu_ns != 0
            || operational.gpu_device_critical_path_ns != 0
            || operational.gpu_timestamp_query_failures != 0
            || operational.gpu_partition_select_dispatches != 0
            || operational.gpu_partition_anchor_merge_dispatches != 0
            || operational.gpu_index_build_candidates != 0
            || operational.gpu_index_build_canonical_emitted != 0
            || operational.gpu_index_build_overlap_duplicates_rejected != 0
            || operational.gpu_index_build_noncanonical_extras_rejected != 0
            || operational.gpu_index_build_surviving_extras != 0
            || operational.gpu_persisted_resident_uploads != 0
            || operational.gpu_persisted_resident_cache_hits != 0
            || operational.gpu_overlap_anchor_duplicates != 0
            || operational.gpu_seed_dispatches != 0
            || operational.gpu_chain_dispatches != 0
            || operational.gpu_chain_backtrack_dispatches != 0
            || operational.gpu_coordinate_vote_dispatches != 0
            || operational.gpu_extend_dispatches != 0
            || operational.gpu_target_window_request_dispatches != 0
            || operational.gpu_target_window_logical_units_requested != 0
            || operational.gpu_queue_submits != 0
            || operational.gpu_host_waits != 0
            || operational.gpu_buffer_creates != 0
            || operational.gpu_memory_allocations != 0
            || operational.gpu_pipeline_creates != 0;
    }

    /// Validate an all-unattested diagnostic without promoting it to stats.
    [[nodiscard]] static constexpr bool is_diagnostic_environment_valid(
        const pipeline_execution_stats& stats) noexcept {
        if (stats.actual_backend != executed_backend::mixed_invalid
            || stats.attempted_alignment_jobs == 0
            || stats.fully_attested_alignment_jobs != 0
            || stats.unattested_alignment_jobs
                != stats.attempted_alignment_jobs) {
            return false;
        }
        if (is_concrete_backend(stats.requested_backend)) {
            return is_environment_valid_for_backend(
                stats.environment, stats.requested_backend);
        }
        if (stats.requested_backend != executed_backend::mixed_invalid) {
            return false;
        }
        const executed_backend environment_backend =
            has_gpu_execution_stats(stats)
            ? executed_backend::vulkan_gpu
            : executed_backend::scalar_cpu;
        return is_environment_valid_for_backend(
            stats.environment, environment_backend);
    }

    /// Validate environment presence before aggregate stats adoption.
    [[nodiscard]] static constexpr bool is_aggregate_environment_operand_valid(
        const pipeline_execution_stats& stats) noexcept {
        if (stats.attempted_alignment_jobs == 0) {
            return execution_environment_snapshot::is_unset(stats.environment);
        }
        if (is_concrete_backend(stats.actual_backend)) {
            return is_environment_valid_for_backend(
                stats.environment, stats.actual_backend);
        }
        // During incremental attestation wiring, or for an intentionally
        // mixed algorithm tuple, execution may publish actual=mixed_invalid.
        // Preserve a truthful environment only when every job is explicitly
        // unattested. This can never satisfy backend acceptance, but it keeps
        // partial-statement diagnostics readable without laundering stats.
        return is_diagnostic_environment_valid(stats);
    }

    /// Validate the exact per-stage aggregate implied by a concrete
    /// backend/source and the number of attempted logical jobs. Optional,
    /// data-dependent stages may cover any subset. Unconditional front-end
    /// stages must cover every job. No forbidden engine or fallback lane may
    /// carry work.
    [[nodiscard]] static constexpr bool is_aggregate_stage_coverage_valid(
        const pipeline_execution_stats& stats) noexcept {
        if (!is_concrete_backend(stats.actual_backend)
            || !is_concrete_target_index_source(
                stats.actual_target_index_source)) {
            return false;
        }
        for (std::size_t i = 0; i < k_pipeline_stage_count; ++i) {
            const auto stage = static_cast<pipeline_stage>(i);
            const stage_coverage& coverage = stats.stages.coverage[i];
            const execution_engine expected_engine = expected_execution_engine(
                stats.actual_backend, stats.actual_target_index_source, stage);
            if (coverage.fallback_jobs != 0
                || coverage.eligible_jobs > stats.attempted_alignment_jobs) {
                return false;
            }
            if (expected_engine == execution_engine::count) {
                if (coverage.eligible_jobs != 0) return false;
                for (const std::uint64_t executed : coverage.executed_jobs) {
                    if (executed != 0) return false;
                }
                continue;
            }
            for (std::size_t engine = 0;
                 engine < coverage.executed_jobs.size(); ++engine) {
                const std::uint64_t expected =
                    engine == engine_index(expected_engine)
                    ? coverage.eligible_jobs
                    : 0;
                if (coverage.executed_jobs[engine] != expected) return false;
            }
            if (is_front_end_eligibility_required(
                    stats.actual_target_index_source, stage)
                && coverage.eligible_jobs
                    != stats.attempted_alignment_jobs) {
                return false;
            }
        }
        return true;
    }

    /// Canonical identity element for stats aggregation. A zero-job value with
    /// selected backend/source labels or a captured environment is not empty;
    /// allowing it to behave as the identity would erase malformed input.
    [[nodiscard]] static constexpr bool is_exact_empty_stats_operand(
        const pipeline_execution_stats& stats) noexcept {
        return stats.attempted_alignment_jobs == 0
            && stats.fully_attested_alignment_jobs == 0
            && stats.unattested_alignment_jobs == 0
            && stats.requested_backend == executed_backend::none
            && stats.actual_backend == executed_backend::none
            && stats.requested_simd_chaining_policy
                == simd_chaining_policy_identity::none
            && stats.observed_simd_chaining_policy
                == simd_chaining_policy_identity::none
            && stats.requested_simd_alignment_algorithm
                == simd_alignment_algorithm_identity::none
            && stats.observed_simd_alignment_algorithm
                == simd_alignment_algorithm_identity::none
            && stats.requested_target_index_source == target_index_source::none
            && stats.actual_target_index_source == target_index_source::none
            && stats.persisted_generation_id == 0
            && stats.persisted_source_identity == 0
            && !stats.has_mixed_persisted_identity
            && stats.first_attempted_target_row == k_unset_job_ordinal
            && stats.first_attempted_query_ordinal == k_unset_job_ordinal
            && stats.first_unattested_target_row == k_unset_job_ordinal
            && stats.first_unattested_query_ordinal == k_unset_job_ordinal
            && execution_environment_snapshot::is_unset(stats.environment)
            && stats.stages == pipeline_job_stage_coverage{}
            && stats.operational == pipeline_operational_stats{};
    }

    /// Only a canonical empty value or an internally complete concrete stats
    /// may retain fully-attested counts across an aggregate merge. Mixed
    /// diagnostics remain readable, but can never promote a clean sibling.
    [[nodiscard]] static constexpr bool can_retain_full_attestation(
        const pipeline_execution_stats& stats) noexcept {
        if (is_exact_empty_stats_operand(stats)) return true;
        if (stats.attempted_alignment_jobs == 0
            || stats.fully_attested_alignment_jobs
                != stats.attempted_alignment_jobs
            || stats.unattested_alignment_jobs != 0
            || stats.first_attempted_target_row == k_unset_job_ordinal
            || stats.first_attempted_query_ordinal == k_unset_job_ordinal
            || stats.first_unattested_target_row != k_unset_job_ordinal
            || stats.first_unattested_query_ordinal != k_unset_job_ordinal
            || stats.requested_backend != stats.actual_backend
            || !is_concrete_backend(stats.actual_backend)
            || !is_simd_chaining_policy_identity_valid_for_backend(
                stats.actual_backend,
                stats.requested_simd_chaining_policy,
                stats.observed_simd_chaining_policy)
            || !is_simd_alignment_algorithm_identity_valid_for_backend(
                stats.actual_backend,
                stats.requested_simd_alignment_algorithm,
                stats.observed_simd_alignment_algorithm)
            || stats.requested_target_index_source
                != stats.actual_target_index_source
            || !is_concrete_target_index_source(
                stats.actual_target_index_source)
            || stats.has_mixed_persisted_identity
            || stats.operational.has_operational_identity_conflict
            || stats.operational.are_stats_saturated
            || stats.operational.backend_declines != 0
            || stats.operational.backend_fallbacks != 0
            || stats.operational.simd_chaining_policy_rejections != 0
            || stats.operational.are_simd_chaining_policy_stats_saturated
            || execution_environment_snapshot::has_no_highway_fields(
                stats.environment)
            || !is_environment_valid_for_backend(
                stats.environment, stats.actual_backend)
            || !is_aggregate_stage_coverage_valid(stats)) {
            return false;
        }
        if (stats.actual_target_index_source == target_index_source::persisted) {
            return stats.persisted_generation_id != 0
                && stats.persisted_generation_id <= k_sql_bigint_value_max
                && stats.persisted_source_identity != 0;
        }
        return stats.persisted_generation_id == 0
            && stats.persisted_source_identity == 0;
    }

    /// Merge one completed buffer/node aggregate without laundering stats gaps.
    void merge(
        const pipeline_execution_stats& rhs) noexcept {
        // Validate both operands before first-stats adoption. In particular, a
        // missing environment on a nonempty left accumulator must not disappear
        // merely because the right operand has a is_valid stats.
        const bool lhs_environment_valid = is_aggregate_environment_operand_valid(*this);
        const bool rhs_environment_valid = is_aggregate_environment_operand_valid(rhs);
        const bool environment_drift = lhs_environment_valid
            && rhs_environment_valid
            && attempted_alignment_jobs != 0
            && rhs.attempted_alignment_jobs != 0
            && !execution_environment_snapshot::has_same_payload_identity(
                environment, rhs.environment);
        const bool lhs_empty = is_exact_empty_stats_operand(*this);
        const bool rhs_empty = is_exact_empty_stats_operand(rhs);
        const bool backend_identity_compatible = lhs_empty || rhs_empty
            || (requested_backend == rhs.requested_backend
                && actual_backend == rhs.actual_backend);
        const bool simd_policy_identity_compatible = lhs_empty || rhs_empty
            || (requested_simd_chaining_policy
                    == rhs.requested_simd_chaining_policy
                && observed_simd_chaining_policy
                    == rhs.observed_simd_chaining_policy);
        const bool simd_alignment_identity_compatible = lhs_empty || rhs_empty
            || (requested_simd_alignment_algorithm
                    == rhs.requested_simd_alignment_algorithm
                && observed_simd_alignment_algorithm
                    == rhs.observed_simd_alignment_algorithm);
        const bool source_identity_compatible = lhs_empty || rhs_empty
            || (requested_target_index_source
                    == rhs.requested_target_index_source
                && actual_target_index_source
                    == rhs.actual_target_index_source);
        const bool persisted_identity_compatible = lhs_empty || rhs_empty
            || (persisted_generation_id == rhs.persisted_generation_id
                && persisted_source_identity == rhs.persisted_source_identity
                && has_mixed_persisted_identity == rhs.has_mixed_persisted_identity);
        // The exact empty operand is a two-sided identity, but two completed
        // statistics that name different nonzero Highway bindings are not
        // compatible stats shards. execution_environment_snapshot::merge()
        // truthfully records that case as mixed; revoke full attestation in
        // this same two-operand merge instead of waiting for a later operand.
        const bool highway_binding_identity_compatible = lhs_empty || rhs_empty
            || (environment.highway_executed_target
                    == rhs.environment.highway_executed_target
                && environment.is_highway_executed_target_mixed
                    == rhs.environment.is_highway_executed_target_mixed);
        const bool retain_full_attestation =
            can_retain_full_attestation(*this)
            && can_retain_full_attestation(rhs)
            && lhs_environment_valid
            && rhs_environment_valid
            && !environment_drift
            && backend_identity_compatible
            && simd_policy_identity_compatible
            && simd_alignment_identity_compatible
            && source_identity_compatible
            && persisted_identity_compatible
            && highway_binding_identity_compatible;

        const std::uint64_t lhs_attempted = attempted_alignment_jobs;
        const std::uint64_t rhs_attempted = rhs.attempted_alignment_jobs;
        const std::uint64_t lhs_fully_attested = retain_full_attestation
            ? fully_attested_alignment_jobs : 0;
        const std::uint64_t lhs_unattested = retain_full_attestation
            ? unattested_alignment_jobs : lhs_attempted;
        const std::uint64_t rhs_fully_attested = retain_full_attestation
            ? rhs.fully_attested_alignment_jobs : 0;
        const std::uint64_t rhs_unattested = retain_full_attestation
            ? rhs.unattested_alignment_jobs : rhs_attempted;

        fully_attested_alignment_jobs = lhs_fully_attested;
        unattested_alignment_jobs = lhs_unattested;
        if (!retain_full_attestation && (lhs_attempted != 0 || rhs_attempted != 0)) {
            if (lhs_attempted != 0) {
                first_unattested_target_row =
                    first_unattested_target_row != k_unset_job_ordinal
                    ? first_unattested_target_row
                    : first_attempted_target_row;
                first_unattested_query_ordinal =
                    first_unattested_query_ordinal != k_unset_job_ordinal
                    ? first_unattested_query_ordinal
                    : first_attempted_query_ordinal;
            } else {
                first_unattested_target_row =
                    rhs.first_unattested_target_row != k_unset_job_ordinal
                    ? rhs.first_unattested_target_row
                    : rhs.first_attempted_target_row;
                first_unattested_query_ordinal =
                    rhs.first_unattested_query_ordinal != k_unset_job_ordinal
                    ? rhs.first_unattested_query_ordinal
                    : rhs.first_attempted_query_ordinal;
            }
        }

        const auto identity_invalid = [](
            target_index_source source,
            std::uint64_t generation,
            std::uint64_t identity) noexcept {
            if (source == target_index_source::persisted) {
                return generation == 0 || identity == 0;
            }
            if (source == target_index_source::recomputed
                || source == target_index_source::none) {
                return generation != 0 || identity != 0;
            }
            return true;
        };
        if (identity_invalid(actual_target_index_source,
                             persisted_generation_id,
                             persisted_source_identity)
            || identity_invalid(rhs.actual_target_index_source,
                                rhs.persisted_generation_id,
                                rhs.persisted_source_identity)) {
            has_mixed_persisted_identity = true;
        }
        requested_backend = merge_backend_identity(requested_backend, rhs.requested_backend);
        actual_backend = merge_backend_identity(actual_backend, rhs.actual_backend);
        requested_simd_chaining_policy = merge_simd_chaining_policy_identity(
            requested_simd_chaining_policy,
            rhs.requested_simd_chaining_policy);
        observed_simd_chaining_policy = merge_simd_chaining_policy_identity(
            observed_simd_chaining_policy,
            rhs.observed_simd_chaining_policy);
        requested_simd_alignment_algorithm =
            merge_simd_alignment_algorithm_identity(
                requested_simd_alignment_algorithm,
                rhs.requested_simd_alignment_algorithm);
        observed_simd_alignment_algorithm =
            merge_simd_alignment_algorithm_identity(
                observed_simd_alignment_algorithm,
                rhs.observed_simd_alignment_algorithm);
        requested_target_index_source = merge_target_index_identity(
            requested_target_index_source, rhs.requested_target_index_source);
        actual_target_index_source = merge_target_index_identity(
            actual_target_index_source, rhs.actual_target_index_source);

        if ((persisted_generation_id == 0) != (persisted_source_identity == 0)
            || (rhs.persisted_generation_id == 0) != (rhs.persisted_source_identity == 0)) {
            has_mixed_persisted_identity = true;
        }
        if (rhs.persisted_generation_id != 0 && rhs.persisted_source_identity != 0) {
            if (persisted_generation_id == 0 && persisted_source_identity == 0) {
                persisted_generation_id = rhs.persisted_generation_id;
                persisted_source_identity = rhs.persisted_source_identity;
            } else if (persisted_generation_id != rhs.persisted_generation_id
                       || persisted_source_identity != rhs.persisted_source_identity) {
                has_mixed_persisted_identity = true;
            }
        }
        has_mixed_persisted_identity = has_mixed_persisted_identity || rhs.has_mixed_persisted_identity;

        if (!execution_environment_snapshot::is_unset(rhs.environment)
            || rhs.attempted_alignment_jobs != 0) {
            environment.merge(rhs.environment);
        }
        if (!lhs_environment_valid || !rhs_environment_valid || environment_drift) {
            environment.is_mixed_invalid = true;
        }

        bool is_saturated = merge_counter(
            attempted_alignment_jobs, rhs.attempted_alignment_jobs);
        is_saturated = merge_counter(
            fully_attested_alignment_jobs, rhs_fully_attested) || is_saturated;
        is_saturated = merge_counter(
            unattested_alignment_jobs, rhs_unattested) || is_saturated;
        is_saturated = stages.merge(rhs.stages) || is_saturated;
        if (first_unattested_target_row == k_unset_job_ordinal
            && rhs.first_unattested_target_row != k_unset_job_ordinal) {
            first_unattested_target_row = rhs.first_unattested_target_row;
            first_unattested_query_ordinal = rhs.first_unattested_query_ordinal;
        }
        if (first_attempted_target_row == k_unset_job_ordinal
            && rhs.first_attempted_target_row != k_unset_job_ordinal) {
            first_attempted_target_row = rhs.first_attempted_target_row;
            first_attempted_query_ordinal = rhs.first_attempted_query_ordinal;
        }
        operational.merge(rhs.operational);
        operational.are_stats_saturated = operational.are_stats_saturated || is_saturated;
        if (operational.are_stats_saturated
            || operational.has_operational_identity_conflict
            || operational.backend_declines != 0
            || operational.backend_fallbacks != 0
            || operational.simd_chaining_policy_rejections != 0
            || operational.are_simd_chaining_policy_stats_saturated
            || requested_simd_chaining_policy
                == simd_chaining_policy_identity::mixed_invalid
            || observed_simd_chaining_policy
                == simd_chaining_policy_identity::mixed_invalid
            || requested_simd_alignment_algorithm
                == simd_alignment_algorithm_identity::mixed_invalid
            || observed_simd_alignment_algorithm
                == simd_alignment_algorithm_identity::mixed_invalid) {
            reclassify_all_attempted_jobs();
        }
    }
};

/// Fixed publication equation checked by the PostgreSQL owner before one
/// persisted grouped-SoA GPU wave becomes visible.  `consumed_jobs` alone is not
/// authority: a mismatched expectation deliberately burns its record, so a
/// stats may be fully consumed while still being ineligible for publication.
struct persisted_grouped_soa_coordinate_publication_stats {
    std::uint64_t expected_jobs = 0;
    std::uint64_t validated_jobs = 0;
    std::uint64_t consumed_jobs = 0;
    std::uint64_t accepted_jobs = 0;
    std::uint64_t burned_jobs = 0;
    std::uint64_t pending_jobs = 0;
    std::uint64_t staged_jobs = 0;
};

[[nodiscard]] constexpr bool
is_persisted_grouped_soa_coordinate_publication_consistent(
    const persisted_grouped_soa_coordinate_publication_stats& counts) noexcept {
    return counts.expected_jobs != 0
        && counts.expected_jobs <= k_sql_bigint_value_max
        && counts.validated_jobs == counts.expected_jobs
        && counts.consumed_jobs == counts.expected_jobs
        && counts.accepted_jobs == counts.expected_jobs
        && counts.burned_jobs == 0
        && counts.pending_jobs == 0
        && counts.staged_jobs == counts.expected_jobs;
}

static_assert(std::is_trivially_copyable_v<pipeline_execution_stats>);
static_assert(std::is_standard_layout_v<pipeline_execution_stats>);

/// Maximum statement-local persisted-GPU records retained for SQL stats.
/// This is the production dispatch ceiling, not a silently growing cache.
inline constexpr std::uint64_t
    k_persisted_gpu_operation_capacity = 8192u;

/// One causal queue submission, represented without Vulkan handles/pointers.
struct persisted_gpu_submission_stats {
    std::uint64_t operation_identity = 0u;
    std::uint64_t ordinal = 0u;
    std::uint64_t submit_wait_identity = 0u;

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        return operation_identity != 0u && ordinal != 0u
            && ordinal <= k_sql_bigint_value_max
            && submit_wait_identity != 0u;
    }
    [[nodiscard]] constexpr bool operator==(
        const persisted_gpu_submission_stats&) const noexcept =
        default;
};

/// Immutable numeric Vulkan/cache identity copied from the runtime stats.
struct persisted_gpu_environment_snapshot {
    std::array<std::uint8_t, 16> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, 16> pipeline_cache_uuid{};
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return api_version != 0u && driver_version != 0u
            && seed_size != 0u && seed_size <= k_sql_bigint_value_max
            && manifest_checksum != 0u
            && std::any_of(
                device_uuid.begin(), device_uuid.end(),
                [](const std::uint8_t value) { return value != 0u; })
            && std::any_of(
                pipeline_cache_uuid.begin(), pipeline_cache_uuid.end(),
                [](const std::uint8_t value) { return value != 0u; })
            && std::any_of(
                seed_sha256.begin(), seed_sha256.end(),
                [](const std::uint8_t value) { return value != 0u; });
    }
    [[nodiscard]] constexpr bool operator==(
        const persisted_gpu_environment_snapshot&) const noexcept = default;
};

inline constexpr std::size_t
    k_persisted_gpu_helper_build_identity_capacity = 128u;
inline constexpr std::size_t k_persisted_gpu_helper_source_capacity = 32u;

/// Process-wide immutable Metal archive qualification copied without paths,
/// file descriptors, API handles or pointers. Text fields are bounded ASCII
/// images so the admitted operation chunk remains a fixed allocation.
struct persisted_gpu_cache_validation_result {
    std::uint32_t qualification_wire_revision = 0u;
    std::uint64_t identity = 0u;
    std::uint64_t archive_count = 0u;
    std::uint64_t archive_bytes = 0u;
    std::uint64_t archive_checksum = 0u;
    std::uint64_t receipt_count = 0u;
    std::uint64_t hash_passes = 0u;
    std::uint64_t hashed_bytes = 0u;
    std::uint64_t private_files_created = 0u;
    std::uint64_t private_files_unlinked = 0u;
    std::uint64_t live_private_paths = 0u;
    std::uint64_t pipeline_filesystem_hash_passes = 0u;
    std::uint64_t pipeline_filesystem_hashed_bytes = 0u;
    std::uint32_t helper_wire_revision = 0u;
    std::array<char, k_persisted_gpu_helper_build_identity_capacity>
        helper_build_identity{};
    std::uint32_t helper_build_identity_size = 0u;
    std::array<char, k_persisted_gpu_helper_source_capacity> helper_source{};
    std::uint32_t helper_source_size = 0u;
    std::uint64_t helper_executable_size = 0u;
    std::array<std::uint8_t, 32> helper_executable_sha256{};
    std::uint64_t helper_metal_registry_id = 0u;
    std::array<std::uint8_t, 16> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, 16> pipeline_cache_uuid{};
    std::uint64_t seed_receipt_identity = 0u;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    bool is_saturated = false;

    [[nodiscard]] std::string_view helper_build_identity_text()
        const noexcept {
        return {helper_build_identity.data(), helper_build_identity_size};
    }
    [[nodiscard]] std::string_view helper_source_text() const noexcept {
        return {helper_source.data(), helper_source_size};
    }

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        const auto nonzero = [](const auto& p_bytes) noexcept {
            return std::any_of(
                p_bytes.begin(), p_bytes.end(),
                [](const auto p_value) { return p_value != 0u; });
        };
        const auto safe_text = [](const auto& p_bytes,
                                  const std::uint32_t p_size) noexcept {
            if (p_size == 0u || p_size > p_bytes.size()) return false;
            for (std::size_t i = 0u; i < p_size; ++i) {
                const auto value = static_cast<unsigned char>(p_bytes[i]);
                if (value < 0x20u || value > 0x7eu
                    || value == static_cast<unsigned char>('"')
                    || value == static_cast<unsigned char>('\\')) {
                    return false;
                }
            }
            return std::all_of(
                p_bytes.begin() + p_size, p_bytes.end(),
                [](const char value) { return value == '\0'; });
        };
        return qualification_wire_revision == 2u && identity != 0u
            && archive_count != 0u
            && archive_count <= k_sql_bigint_value_max
            && archive_bytes != 0u
            && archive_bytes <= k_sql_bigint_value_max
            && archive_checksum != 0u
            && receipt_count == archive_count
            && hash_passes == 1u && hashed_bytes == archive_bytes
            && private_files_created == archive_count
            && private_files_unlinked == archive_count
            && live_private_paths == 0u
            && pipeline_filesystem_hash_passes == 0u
            && pipeline_filesystem_hashed_bytes == 0u
            && helper_wire_revision == 2u
            && safe_text(
                helper_build_identity, helper_build_identity_size)
            && helper_build_identity_text()
                == "bioseq_gpu_archive_builder"
            && safe_text(helper_source, helper_source_size)
            && helper_source_text() == "installed-sibling"
            && helper_executable_size != 0u
            && helper_executable_size <= k_sql_bigint_value_max
            && nonzero(helper_executable_sha256)
            && helper_metal_registry_id != 0u
            && nonzero(device_uuid) && api_version != 0u
            && driver_version != 0u
            && nonzero(pipeline_cache_uuid)
            && seed_receipt_identity != 0u && nonzero(seed_sha256)
            && seed_size != 0u && seed_size <= k_sql_bigint_value_max
            && manifest_checksum != 0u && !is_saturated;
    }
    [[nodiscard]] constexpr bool operator==(
        const persisted_gpu_cache_validation_result&) const noexcept =
        default;
};

/// Report-once cache counters attached to the is_accepted operation authority.
struct persisted_gpu_cache_metrics {
    std::uint64_t token_identity = 0u;
    std::uint64_t pipeline_cache_loads = 0u;
    std::uint64_t pipeline_cache_seed_bytes = 0u;
    std::uint64_t pipeline_cache_load_ns = 0u;
    std::uint64_t pipeline_creates = 0u;
    std::uint64_t pipeline_creation_ns = 0u;

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        const bool load_shape = pipeline_cache_loads == 0u
            ? pipeline_cache_seed_bytes == 0u
                && pipeline_cache_load_ns == 0u
            : pipeline_cache_loads == 1u
                && pipeline_cache_seed_bytes != 0u
                && pipeline_cache_load_ns != 0u;
        const bool create_shape = pipeline_creates == 0u
            ? pipeline_creation_ns == 0u
            : pipeline_creation_ns != 0u;
        return token_identity != 0u && load_shape && create_shape
            && pipeline_cache_loads <= k_sql_bigint_value_max
            && pipeline_cache_seed_bytes <= k_sql_bigint_value_max
            && pipeline_cache_load_ns <= k_sql_bigint_value_max
            && pipeline_creates <= k_sql_bigint_value_max
            && pipeline_creation_ns <= k_sql_bigint_value_max;
    }
    [[nodiscard]] constexpr bool operator==(
        const persisted_gpu_cache_metrics&) const noexcept = default;
};

/// Coordinate result copied after the runtime's status/readback checks.
struct persisted_gpu_coordinate_result_stats {
    std::uint64_t query_start = 0u;
    std::uint64_t query_end = 0u;
    std::uint64_t target_start = 0u;
    std::uint64_t target_end = 0u;
    std::uint64_t match_length = 0u;
    std::uint64_t block_length = 0u;
    std::int32_t chain_score = 0;
    std::int32_t seed_count = 0;
    bool is_reverse = false;
    bool is_valid = false;

    [[nodiscard]] constexpr bool is_internally_consistent(
        const std::uint64_t query_length,
        const std::uint64_t target_length) const noexcept {
        return is_valid && chain_score > 0 && seed_count > 0
            && query_start < query_end && query_end <= query_length
            && target_start < target_end && target_end <= target_length
            && match_length != 0u
            && match_length <= block_length
            && match_length <= query_end - query_start
            && match_length <= target_end - target_start
            && block_length <= query_end - query_start
                + target_end - target_start
            && query_start <= k_sql_bigint_value_max
            && query_end <= k_sql_bigint_value_max
            && target_start <= k_sql_bigint_value_max
            && target_end <= k_sql_bigint_value_max
            && match_length <= k_sql_bigint_value_max
            && block_length <= k_sql_bigint_value_max;
    }
};

/// Full-stage causal statistics for one logical target/query coordinate result.
struct persisted_gpu_job_stats {
    std::uint64_t logical_target_row = 0u;
    std::uint64_t logical_query_ordinal = 0u;
    std::uint32_t query_index = 0u;
    std::uint64_t query_length = 0u;
    std::uint64_t target_length = 0u;
    std::uint64_t query_minimizer_count = 0u;
    std::uint64_t query_tile_count = 0u;
    std::uint64_t query_count_dispatches = 0u;
    std::uint64_t query_scan_dispatches = 0u;
    std::uint64_t query_emit_dispatches = 0u;
    std::uint64_t query_completed_roundtrips = 0u;
    std::uint64_t direct_count_dispatches = 0u;
    std::uint64_t direct_scan_dispatches = 0u;
    std::uint64_t anchor_count = 0u;
    std::uint64_t direct_emit_dispatches = 0u;
    std::uint64_t sort_operations = 0u;
    std::uint64_t range_build_dispatches = 0u;
    std::uint64_t range_count = 0u;
    std::uint64_t chain_dispatches = 0u;
    std::uint64_t chain_backtrack_dispatches = 0u;
    std::uint64_t coordinate_dispatches = 0u;
    std::array<persisted_gpu_submission_stats, 4> submissions{};
    std::uint64_t count_status_checks = 0u;
    std::uint64_t emit_status_checks = 0u;
    std::uint32_t status_or = 0u;
    std::uint64_t result_readbacks = 0u;
    std::uint64_t coordinate_vote_dispatches = 0u;
    std::uint64_t cpu_or_simd_biological_calls = 0u;
    std::uint64_t recompute_calls = 0u;
    std::uint64_t fallback_calls = 0u;
    persisted_gpu_coordinate_result_stats coordinate{};

    [[nodiscard]] constexpr bool is_internally_consistent(
        const std::uint64_t shape_operation_identity) const noexcept {
        if (logical_target_row > k_sql_bigint_value_max
            || logical_query_ordinal > k_sql_bigint_value_max
            || query_length == 0u || query_length > k_sql_bigint_value_max
            || target_length == 0u || target_length > k_sql_bigint_value_max
            || query_minimizer_count == 0u
            || query_minimizer_count > k_sql_bigint_value_max
            || query_tile_count == 0u
            || query_tile_count > k_sql_bigint_value_max
            || query_count_dispatches != 1u
            || query_scan_dispatches != 3u
            || query_emit_dispatches != 1u
            || query_completed_roundtrips != 2u
            || direct_count_dispatches != 1u
            || direct_scan_dispatches != 3u
            || anchor_count == 0u || anchor_count > k_sql_bigint_value_max
            || direct_emit_dispatches != 1u
            || sort_operations != 1u
            || range_build_dispatches != 1u
            || range_count == 0u || range_count > k_sql_bigint_value_max
            || chain_dispatches != 1u
            || chain_backtrack_dispatches != 1u
            || coordinate_dispatches != 1u
            || count_status_checks != 1u
            || emit_status_checks != 1u
            || status_or != 0u || result_readbacks != 1u
            || coordinate_vote_dispatches != 0u
            || cpu_or_simd_biological_calls != 0u
            || recompute_calls != 0u || fallback_calls != 0u
            || !coordinate.is_internally_consistent(query_length, target_length)) {
            return false;
        }
        for (std::size_t i = 0u; i < submissions.size(); ++i) {
            if (!submissions[i].is_internally_consistent()
                || submissions[i].operation_identity
                    != shape_operation_identity) {
                return false;
            }
            for (std::size_t prior = 0u; prior < i; ++prior) {
                if (submissions[prior].ordinal == submissions[i].ordinal
                    || submissions[prior].submit_wait_identity
                        == submissions[i].submit_wait_identity) {
                    return false;
                }
            }
        }
        return true;
    }
};

/// One publication-wave envelope. The final cache delta may be replaced only
/// by the runtime authority immediately before allocation-free ledger linking.
struct persisted_gpu_operation_stats {
    std::uint64_t operation_identity = 0u;
    persisted_grouped_soa_coordinate_publication_stats publication{};
    std::uint64_t published_jobs = 0u;
    std::uint64_t publication_commits = 0u;
    std::uint64_t query_pipeline_receipt_count = 0u;
    std::uint64_t shape_pipeline_receipt_count = 0u;
    std::uint64_t submission_count = 0u;
    std::uint64_t query_pipeline_operation_identity = 0u;
    std::uint64_t shape_pipeline_operation_identity = 0u;
    std::array<std::uint64_t, 2> query_submission_ordinals{};
    std::uint64_t seed_receipt_identity = 0u;
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t cold_load_identity = 0u;
    persisted_gpu_environment_snapshot environment{};
    persisted_gpu_cache_validation_result cache_qualification{};
    persisted_gpu_cache_metrics cache_metrics{};

    [[nodiscard]] bool is_internally_consistent(
        const std::span<const persisted_gpu_job_stats> jobs) const noexcept {
        if (operation_identity == 0u
            || !is_persisted_grouped_soa_coordinate_publication_consistent(
                publication)
            || published_jobs != publication.expected_jobs
            || publication_commits != 1u
            || query_pipeline_receipt_count != 3u
            || shape_pipeline_receipt_count < 7u
            || shape_pipeline_receipt_count > 10u
            || submission_count < 6u
            || submission_count > k_sql_bigint_value_max
            || query_pipeline_operation_identity == 0u
            || shape_pipeline_operation_identity == 0u
            || shape_pipeline_operation_identity != operation_identity
            || query_pipeline_operation_identity
                == shape_pipeline_operation_identity
            || query_submission_ordinals[0] == 0u
            || query_submission_ordinals[1] == 0u
            || query_submission_ordinals[0]
                == query_submission_ordinals[1]
            || query_submission_ordinals[0] > k_sql_bigint_value_max
            || query_submission_ordinals[1] > k_sql_bigint_value_max
            || seed_receipt_identity == 0u
            || generation_id == 0u
            || generation_id > k_sql_bigint_value_max
            || source_identity == 0u || cold_load_identity == 0u
            || !environment.is_internally_consistent() || !cache_qualification.is_internally_consistent()
            || cache_qualification.device_uuid != environment.device_uuid
            || cache_qualification.api_version != environment.api_version
            || cache_qualification.driver_version
                != environment.driver_version
            || cache_qualification.vendor_id != environment.vendor_id
            || cache_qualification.device_id != environment.device_id
            || cache_qualification.pipeline_cache_uuid
                != environment.pipeline_cache_uuid
            || cache_qualification.seed_receipt_identity
                != seed_receipt_identity
            || cache_qualification.seed_sha256 != environment.seed_sha256
            || cache_qualification.seed_size != environment.seed_size
            || cache_qualification.manifest_checksum
                != environment.manifest_checksum
            || !cache_metrics.is_internally_consistent()
            || cache_metrics.token_identity != operation_identity
            || jobs.size() != publication.expected_jobs) {
            return false;
        }
        std::uint64_t shape_ordinal_count = 0u;
        for (std::size_t job_index = 0u; job_index < jobs.size(); ++job_index) {
            const persisted_gpu_job_stats& job = jobs[job_index];
            if (!job.is_internally_consistent(shape_pipeline_operation_identity)) return false;
            for (std::size_t prior = 0u; prior < job_index; ++prior) {
                if (jobs[prior].logical_target_row == job.logical_target_row
                    && jobs[prior].logical_query_ordinal
                        == job.logical_query_ordinal) {
                    return false;
                }
            }
            for (std::size_t submission_index = 0u;
                 submission_index < job.submissions.size();
                 ++submission_index) {
                const auto& submission = job.submissions[submission_index];
                bool previously_seen = false;
                for (std::size_t prior_job = 0u;
                     prior_job <= job_index && !previously_seen;
                     ++prior_job) {
                    const std::size_t prior_submission_limit =
                        prior_job == job_index
                        ? submission_index
                        : jobs[prior_job].submissions.size();
                    for (std::size_t prior_submission = 0u;
                         prior_submission < prior_submission_limit;
                         ++prior_submission) {
                        const auto& prior =
                            jobs[prior_job].submissions[prior_submission];
                        const bool same_ordinal =
                            prior.ordinal == submission.ordinal;
                        const bool same_wait = prior.submit_wait_identity
                            == submission.submit_wait_identity;
                        if ((same_ordinal || same_wait)
                            && !(same_ordinal && same_wait
                                && prior_submission == submission_index)) {
                            return false;
                        }
                        previously_seen = same_ordinal;
                        if (previously_seen) break;
                    }
                }
                if (!previously_seen) {
                    if (shape_ordinal_count == k_sql_bigint_value_max) {
                        return false;
                    }
                    ++shape_ordinal_count;
                }
            }
        }
        return submission_count == 2u + shape_ordinal_count;
    }
};

class persisted_gpu_operation_accumulator;

/// Pre-finalizer authority proving one sealed chunk can be linked without any
/// later allocation, duplicate search, range check, or capacity decision.
class persisted_gpu_operation_append_authorization {
public:
    persisted_gpu_operation_append_authorization() = default;
    persisted_gpu_operation_append_authorization(
        const persisted_gpu_operation_append_authorization&) = delete;
    persisted_gpu_operation_append_authorization& operator=(
        const persisted_gpu_operation_append_authorization&) = delete;
    persisted_gpu_operation_append_authorization(
        persisted_gpu_operation_append_authorization&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)) {}
    persisted_gpu_operation_append_authorization& operator=(
        persisted_gpu_operation_append_authorization&& other) noexcept {
        if (this != &other) {
            owner_ = std::exchange(other.owner_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept { return owner_ != nullptr; }

private:
    explicit persisted_gpu_operation_append_authorization(
        persisted_gpu_operation_accumulator& owner) noexcept
        : owner_(&owner) {}

    persisted_gpu_operation_accumulator* owner_ = nullptr;
    friend class persisted_gpu_operation_accumulator;
};

/// One admitted operation chunk. The job vector dies before its permit, and
/// the ledger removes next links iteratively before destroying each chunk.
class persisted_gpu_operation_chunk {
public:
    persisted_gpu_operation_chunk() = default;
    persisted_gpu_operation_chunk(
        const persisted_gpu_operation_chunk&) = delete;
    persisted_gpu_operation_chunk& operator=(
        const persisted_gpu_operation_chunk&) = delete;
    persisted_gpu_operation_chunk(
        persisted_gpu_operation_chunk&&) = delete;
    persisted_gpu_operation_chunk& operator=(
        persisted_gpu_operation_chunk&&) = delete;

    /// Admit and allocate one exact operation/job image before finalization.
    [[nodiscard]] static std::unique_ptr<
        persisted_gpu_operation_chunk> prepare(
            const std::size_t job_count) noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (job_count == 0u
            || job_count > k_persisted_gpu_operation_capacity
            || job_count > std::vector<persisted_gpu_job_stats>{}.max_size()) {
            return nullptr;
        }
        const std::uint64_t retained = saturating_add(
            sizeof(persisted_gpu_operation_chunk),
            saturating_mul(
                static_cast<std::uint64_t>(job_count),
                sizeof(persisted_gpu_job_stats)));
        const std::uint64_t peak = saturating_mul(
            retained,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || peak == (std::numeric_limits<std::uint64_t>::max)()) {
            return nullptr;
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(peak, &failure);
        if (!permit) return nullptr;
        try {
            auto chunk = std::make_unique<
                persisted_gpu_operation_chunk>();
            chunk->capacity_permit_ = std::move(*permit);
            chunk->jobs_.resize(job_count);
            const std::uint64_t actual = saturating_add(
                sizeof(persisted_gpu_operation_chunk),
                saturating_mul(
                    static_cast<std::uint64_t>(chunk->jobs_.capacity()),
                    sizeof(persisted_gpu_job_stats)));
            if (actual == (std::numeric_limits<std::uint64_t>::max)()
                || actual > chunk->capacity_permit_.bytes()) {
                return nullptr;
            }
            chunk->capacity_permit_.shrink_to(actual);
            return chunk;
        } catch (...) {
            return nullptr;
        }
    }

    [[nodiscard]] persisted_gpu_operation_stats& operation() noexcept {
        return operation_;
    }
    [[nodiscard]] std::span<persisted_gpu_job_stats> jobs() noexcept {
        return jobs_;
    }
    [[nodiscard]] const persisted_gpu_operation_stats& operation()
        const noexcept { return operation_; }
    [[nodiscard]] std::span<const persisted_gpu_job_stats> jobs()
        const noexcept { return jobs_; }
    [[nodiscard]] const persisted_gpu_operation_chunk* next()
        const noexcept { return next_.get(); }

    /// Freeze populated fields after every full-stage/range/equation check.
    [[nodiscard]] bool seal() noexcept {
        is_sealed_ = operation_.is_internally_consistent(jobs_);
        return is_sealed_;
    }
    [[nodiscard]] bool is_sealed() const noexcept {
        return is_sealed_;
    }

    /// Publish the runtime-prevalidated final report-once delta. runtime's
    /// claim plan proves componentwise boundedness before its irreversible
    /// report-once commit, so this post-finalizer step is only a fixed write.
    void publish_cache_metrics(
        const persisted_gpu_cache_metrics& metrics) noexcept {
        operation_.cache_metrics = metrics;
        has_published_cache_metrics_ = true;
    }
    [[nodiscard]] bool has_published_cache_metrics() const noexcept {
        return has_published_cache_metrics_;
    }

private:
    // Declared first: jobs/next are destroyed before their admission permit.
    bioseq::parallelism::memory_permit capacity_permit_{};
    std::unique_ptr<persisted_gpu_operation_chunk> next_{};
    std::vector<persisted_gpu_job_stats> jobs_{};
    persisted_gpu_operation_stats operation_{};
    bool has_published_cache_metrics_ = false;
    bool is_sealed_ = false;
    friend class persisted_gpu_operation_accumulator;
};

/// Statement-root-owned causal statistics ledger. It never owns through stats;
/// schedulers retain only a borrowed pointer to this root object.
class persisted_gpu_operation_accumulator {
public:
    persisted_gpu_operation_accumulator() = default;
    persisted_gpu_operation_accumulator(
        const persisted_gpu_operation_accumulator&) = delete;
    persisted_gpu_operation_accumulator& operator=(
        const persisted_gpu_operation_accumulator&) = delete;
    persisted_gpu_operation_accumulator(
        persisted_gpu_operation_accumulator&& other) noexcept {
        move_from(other);
    }
    persisted_gpu_operation_accumulator& operator=(
        persisted_gpu_operation_accumulator&& other) noexcept {
        if (this != &other) {
            clear_iterative();
            move_from(other);
        }
        return *this;
    }
    ~persisted_gpu_operation_accumulator() { clear_iterative(); }

    /// Complete all duplicate/capacity/range checks before runtime finalizer.
    [[nodiscard]] persisted_gpu_operation_append_authorization prepare_append(
        const std::unique_ptr<persisted_gpu_operation_chunk>& chunk)
        noexcept {
        if (is_frozen_ || is_invalid_ || chunk == nullptr || !chunk->is_sealed()
            || chunk->has_published_cache_metrics()
            || operation_count_ >= k_persisted_gpu_operation_capacity
            || job_count_ > k_persisted_gpu_operation_capacity
                - chunk->jobs().size()) {
            invalidate(chunk == nullptr ? 0u : 1u, true);
            return {};
        }
        const auto& operation = chunk->operation();
        for (const persisted_gpu_operation_chunk* existing = head_.get();
             existing != nullptr; existing = existing->next()) {
            const auto& existing_operation = existing->operation();
            if (existing_operation.operation_identity
                    == operation.operation_identity
                || existing_operation.query_pipeline_operation_identity
                    == operation.query_pipeline_operation_identity
                || existing_operation.query_pipeline_operation_identity
                    == operation.shape_pipeline_operation_identity
                || existing_operation.shape_pipeline_operation_identity
                    == operation.query_pipeline_operation_identity) {
                invalidate(1u, false);
                return {};
            }
            if (existing_operation.generation_id != operation.generation_id
                || existing_operation.source_identity != operation.source_identity
                || existing_operation.cold_load_identity
                    != operation.cold_load_identity
                || existing_operation.seed_receipt_identity
                    != operation.seed_receipt_identity
                || existing_operation.environment != operation.environment
                || existing_operation.cache_qualification
                    != operation.cache_qualification) {
                invalidate(1u, false);
                return {};
            }
        }
        return persisted_gpu_operation_append_authorization{*this};
    }

    /// Allocation-free/noexcept link after is_accepted runtime authority exists.
    void commit_append(
        persisted_gpu_operation_append_authorization&& authority,
        std::unique_ptr<persisted_gpu_operation_chunk>&& chunk) noexcept {
        if (authority.owner_ != this
            || chunk == nullptr
            || !chunk->is_sealed()
            || !chunk->has_published_cache_metrics()) {
            invalidate(chunk == nullptr ? 0u : 1u, true);
            authority.owner_ = nullptr;
            return;
        }
        persisted_gpu_operation_chunk* appended = chunk.get();
        if (tail_ == nullptr) {
            head_ = std::move(chunk);
        } else {
            tail_->next_ = std::move(chunk);
        }
        tail_ = appended;
        ++operation_count_;
        job_count_ += tail_->jobs_.size();
        authority.owner_ = nullptr;
    }

    /// Freeze at ExecutorFinish before move into the immutable snapshot.
    void freeze() noexcept { is_frozen_ = true; }

    [[nodiscard]] bool is_complete() const noexcept {
        if (!is_frozen_ || is_invalid_ || is_saturated_ || dropped_operation_count_ != 0u
            || operation_count_ > k_persisted_gpu_operation_capacity
            || job_count_ > k_persisted_gpu_operation_capacity) {
            return false;
        }
        std::uint64_t operations = 0u;
        std::uint64_t jobs = 0u;
        for (const persisted_gpu_operation_chunk* chunk = head_.get();
             chunk != nullptr; chunk = chunk->next()) {
            if (!chunk->is_sealed() || !chunk->has_published_cache_metrics()) {
                return false;
            }
            ++operations;
            jobs += chunk->jobs().size();
        }
        return operations == operation_count_ && jobs == job_count_;
    }
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return operation_count_ != 0u && job_count_ != 0u && is_complete();
    }
    /// Bind the immutable ledger back to the canonical statement aggregate.
    [[nodiscard]] bool is_pipeline_match(
        const pipeline_execution_stats& stats) const noexcept {
        if (!is_internally_consistent()
            || !pipeline_execution_stats::can_retain_full_attestation(stats)
            || stats.actual_backend != executed_backend::vulkan_gpu
            || stats.actual_target_index_source != target_index_source::persisted
            || stats.attempted_alignment_jobs != job_count_
            || stats.fully_attested_alignment_jobs != job_count_
            || stats.persisted_generation_id == 0u
            || stats.persisted_source_identity == 0u
            || !is_environment_valid_for_backend(
                stats.environment, executed_backend::vulkan_gpu)) {
            return false;
        }
        std::uint64_t cache_loads = 0u;
        std::uint64_t cache_seed_bytes = 0u;
        std::uint64_t cache_load_ns = 0u;
        std::uint64_t pipeline_creates = 0u;
        std::uint64_t pipeline_creation_ns = 0u;
        std::uint64_t seed_dispatches = 0u;
        std::uint64_t chain_dispatches = 0u;
        std::uint64_t backtrack_dispatches = 0u;
        const auto add = [](std::uint64_t& destination,
                            const std::uint64_t value) noexcept {
            if (value > (std::numeric_limits<std::uint64_t>::max)()
                    - destination) {
                return false;
            }
            destination += value;
            return true;
        };
        for (const persisted_gpu_operation_chunk* chunk = head_.get();
             chunk != nullptr; chunk = chunk->next()) {
            const auto& operation = chunk->operation();
            ::bioseq::gpu::vulkan::vulkan_feature_set identity{};
            identity.api_version = operation.environment.api_version;
            identity.driver_version = operation.environment.driver_version;
            identity.vendor_id = operation.environment.vendor_id;
            identity.device_id = operation.environment.device_id;
            identity.pipeline_cache_uuid =
                operation.environment.pipeline_cache_uuid;
            if (operation.generation_id != stats.persisted_generation_id
                || operation.source_identity
                    != stats.persisted_source_identity
                || operation.environment.device_uuid
                    != stats.environment.gpu_device_uuid
                || operation.environment.seed_sha256
                    != stats.environment.gpu_pipeline_cache_seed_sha256
                || operation.environment.seed_size
                    != stats.environment.gpu_pipeline_cache_seed_size
                || operation.environment.manifest_checksum
                    != stats.environment
                        .gpu_pipeline_cache_manifest_checksum
                || !operation.cache_qualification.is_internally_consistent()
                || operation.cache_qualification.device_uuid
                    != operation.environment.device_uuid
                || operation.cache_qualification.pipeline_cache_uuid
                    != operation.environment.pipeline_cache_uuid
                || operation.cache_qualification.seed_sha256
                    != operation.environment.seed_sha256
                || operation.cache_qualification.seed_size
                    != operation.environment.seed_size
                || operation.cache_qualification.manifest_checksum
                    != operation.environment.manifest_checksum
                || operation.cache_qualification.seed_receipt_identity
                    != operation.seed_receipt_identity
                || ::bioseq::gpu::vulkan::
                    canonical_vulkan_driver_identity(identity)
                    != stats.environment.gpu_driver_identity
                || !add(cache_loads,
                    operation.cache_metrics.pipeline_cache_loads)
                || !add(cache_seed_bytes,
                    operation.cache_metrics.pipeline_cache_seed_bytes)
                || !add(cache_load_ns,
                    operation.cache_metrics.pipeline_cache_load_ns)
                || !add(pipeline_creates,
                    operation.cache_metrics.pipeline_creates)
                || !add(pipeline_creation_ns,
                    operation.cache_metrics.pipeline_creation_ns)) {
                return false;
            }
            for (const auto& job : chunk->jobs()) {
                if (!add(seed_dispatches,
                        job.direct_count_dispatches
                            + job.direct_emit_dispatches)
                    || !add(chain_dispatches, job.chain_dispatches)
                    || !add(backtrack_dispatches,
                        job.chain_backtrack_dispatches)) {
                    return false;
                }
            }
        }
        return cache_loads
                == stats.operational.gpu_pipeline_cache_loads
            && cache_seed_bytes
                == stats.operational.gpu_pipeline_cache_seed_bytes
            && cache_load_ns
                == stats.operational.gpu_pipeline_cache_load_ns
            && pipeline_creates
                == stats.operational.gpu_pipeline_creates
            && pipeline_creation_ns
                == stats.operational.gpu_pipeline_creation_ns
            && seed_dispatches == stats.operational.gpu_seed_dispatches
            && chain_dispatches == stats.operational.gpu_chain_dispatches
            && backtrack_dispatches
                == stats.operational.gpu_chain_backtrack_dispatches;
    }
    [[nodiscard]] bool is_saturated() const noexcept { return is_saturated_; }
    [[nodiscard]] std::uint64_t dropped_operation_count() const noexcept {
        return dropped_operation_count_;
    }
    [[nodiscard]] std::uint64_t operation_count() const noexcept {
        return operation_count_;
    }
    [[nodiscard]] std::uint64_t job_count() const noexcept {
        return job_count_;
    }
    [[nodiscard]] const persisted_gpu_operation_chunk* head()
        const noexcept { return head_.get(); }

private:
    void invalidate(
        const std::uint64_t dropped,
        const bool is_saturated) noexcept {
        is_invalid_ = true;
        is_saturated_ = is_saturated_ || is_saturated;
        if (dropped > (std::numeric_limits<std::uint64_t>::max)()
                - dropped_operation_count_) {
            dropped_operation_count_ =
                (std::numeric_limits<std::uint64_t>::max)();
            is_saturated_ = true;
        } else {
            dropped_operation_count_ += dropped;
        }
    }

    void clear_iterative() noexcept {
        tail_ = nullptr;
        while (head_ != nullptr) {
            auto next = std::move(head_->next_);
            head_ = std::move(next);
        }
    }

    void move_from(persisted_gpu_operation_accumulator& other) noexcept {
        head_ = std::move(other.head_);
        tail_ = std::exchange(other.tail_, nullptr);
        operation_count_ = std::exchange(other.operation_count_, 0u);
        job_count_ = std::exchange(other.job_count_, 0u);
        dropped_operation_count_ = std::exchange(
            other.dropped_operation_count_, 0u);
        is_invalid_ = std::exchange(other.is_invalid_, false);
        is_saturated_ = std::exchange(other.is_saturated_, false);
        is_frozen_ = std::exchange(other.is_frozen_, false);
    }

    std::unique_ptr<persisted_gpu_operation_chunk> head_{};
    persisted_gpu_operation_chunk* tail_ = nullptr;
    std::uint64_t operation_count_ = 0u;
    std::uint64_t job_count_ = 0u;
    std::uint64_t dropped_operation_count_ = 0u;
    bool is_invalid_ = false;
    bool is_saturated_ = false;
    bool is_frozen_ = false;
};

static_assert(!std::is_copy_constructible_v<
    persisted_gpu_operation_accumulator>);
static_assert(std::is_nothrow_move_constructible_v<
    persisted_gpu_operation_accumulator>);

/// Convert the runtime-minted fixed GPU identity into the statement stats
/// without allocating or inferring cache state from descriptive counters.
/// Only the immutable all-hit policy is_accepted by the persisted operation can
/// become an all-GPU execution environment.
[[nodiscard]] inline std::optional<execution_environment_snapshot>
make_persisted_grouped_soa_gpu_environment_snapshot(
    const ::bioseq::gpu::vulkan::runtime_gpu_execution_environment_snapshot&
        binding,
    const std::size_t effective_cpu_workers) noexcept {
    using runtime_policy =
        ::bioseq::gpu::vulkan::runtime_pipeline_cache_policy;
    if (!binding.is_internally_consistent()
        || binding.cache_policy
            != runtime_policy::immutable_preseed_all_hit
        || effective_cpu_workers == 0
        || effective_cpu_workers > k_sql_integer_value_max
        || binding.seed_size > k_sql_bigint_value_max) {
        return std::nullopt;
    }
    execution_environment_snapshot environment{};
    environment.effective_cpu_workers =
        static_cast<std::uint32_t>(effective_cpu_workers);
    environment.is_gpu_environment_available = binding.gpu_environment_present;
    environment.gpu_device_uuid = binding.gpu_device_uuid;
    environment.gpu_driver_identity = binding.gpu_driver_identity;
    environment.gpu_pipeline_cache_policy =
        pipeline_cache_policy::immutable_preseed_all_hit;
    environment.gpu_pipeline_cache_seed_sha256 = binding.seed_sha256;
    environment.gpu_pipeline_cache_seed_size = binding.seed_size;
    environment.gpu_pipeline_cache_manifest_checksum =
        binding.manifest_checksum;
    environment.gpu_pipeline_cache_hit = binding.pipeline_cache_all_hit
        ? optional_observation_bool::true_value
        : optional_observation_bool::false_value;
    return is_environment_valid_for_backend(
            environment, executed_backend::vulkan_gpu)
        ? std::optional<execution_environment_snapshot>{environment}
        : std::nullopt;
}

/// Convert the separately-owned report-once cache token into an operational
/// delta.  The descriptive environment stats is intentionally insufficient to
/// manufacture these counters, and the token identity itself is not replayed
/// into the SQL aggregate.
[[nodiscard]] inline std::optional<pipeline_operational_stats>
make_persisted_grouped_soa_gpu_cache_metrics_delta(
    const ::bioseq::gpu::vulkan::runtime_gpu_cache_metrics_audit& audit) noexcept {
    if (!audit.is_internally_consistent()
        || audit.gpu_pipeline_cache_loads > k_sql_bigint_value_max
        || audit.gpu_pipeline_cache_seed_bytes > k_sql_bigint_value_max
        || audit.gpu_pipeline_cache_load_ns > k_sql_bigint_value_max
        || audit.gpu_pipeline_creates > k_sql_bigint_value_max
        || audit.gpu_pipeline_creation_ns > k_sql_bigint_value_max) {
        return std::nullopt;
    }
    pipeline_operational_stats delta{};
    delta.gpu_pipeline_cache_loads = audit.gpu_pipeline_cache_loads;
    delta.gpu_pipeline_cache_seed_bytes =
        audit.gpu_pipeline_cache_seed_bytes;
    delta.gpu_pipeline_cache_load_ns = audit.gpu_pipeline_cache_load_ns;
    delta.gpu_pipeline_creates = audit.gpu_pipeline_creates;
    delta.gpu_pipeline_creation_ns = audit.gpu_pipeline_creation_ns;
    return delta;
}

/// The runtime's sealed stats exposes an immutable environment preview before
/// it commits report-once cache bits.  Final authority must reproduce every
/// fixed field exactly. Descriptive equivalence is not enough.
[[nodiscard]] inline bool
is_persisted_grouped_soa_gpu_environment_consistent(
    const ::bioseq::gpu::vulkan::runtime_gpu_execution_environment_snapshot& actual,
    const ::bioseq::gpu::vulkan::runtime_gpu_execution_environment_snapshot&
        preview) noexcept {
    return actual.is_internally_consistent() && preview.is_internally_consistent()
        && actual == preview;
}

/// The pre-finalizer cache audit is a componentwise maximum claim, not the
/// counter delta to publish.  An is_accepted final report-once token may publish
/// only the same operation identity and values bounded by that validated
/// maximum.
[[nodiscard]] inline bool
is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
    const ::bioseq::gpu::vulkan::runtime_gpu_cache_metrics_audit& actual,
    const ::bioseq::gpu::vulkan::runtime_gpu_cache_metrics_audit& preview) noexcept {
    return actual.is_componentwise_bounded_by(preview);
}

/// Validate a staged persisted-GPU stats delta and the prospective aggregate
/// without mutating the caller.  The scheduler performs this check before the
/// allocation-free commit so environment or persisted-generation drift cannot
/// revoke earlier jobs after any result/source/lifecycle state is published.
[[nodiscard]] constexpr bool
is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
    const pipeline_execution_stats& before,
    const pipeline_execution_stats& staged,
    const pipeline_execution_stats& prospective,
    const std::uint64_t expected_jobs) noexcept {
    if (expected_jobs == 0 || expected_jobs > k_sql_bigint_value_max
        || before.attempted_alignment_jobs
            > k_sql_bigint_value_max - expected_jobs) {
        return false;
    }
    const std::uint64_t expected_total =
        before.attempted_alignment_jobs + expected_jobs;
    return pipeline_execution_stats::can_retain_full_attestation(before)
        && pipeline_execution_stats::can_retain_full_attestation(staged)
        && pipeline_execution_stats::can_retain_full_attestation(prospective)
        && staged.requested_backend == executed_backend::vulkan_gpu
        && staged.actual_backend == executed_backend::vulkan_gpu
        && staged.requested_target_index_source
            == target_index_source::persisted
        && staged.actual_target_index_source == target_index_source::persisted
        && staged.attempted_alignment_jobs == expected_jobs
        && staged.fully_attested_alignment_jobs == expected_jobs
        && staged.unattested_alignment_jobs == 0
        && prospective.attempted_alignment_jobs == expected_total
        && prospective.fully_attested_alignment_jobs == expected_total
        && prospective.unattested_alignment_jobs == 0
        && prospective.requested_backend == executed_backend::vulkan_gpu
        && prospective.actual_backend == executed_backend::vulkan_gpu
        && prospective.requested_target_index_source
            == target_index_source::persisted
        && prospective.actual_target_index_source
            == target_index_source::persisted
        && prospective.persisted_generation_id
            == staged.persisted_generation_id
        && prospective.persisted_source_identity
            == staged.persisted_source_identity
        && execution_environment_snapshot::is_payload_equal(
            prospective.environment, staged.environment)
        && !prospective.environment.is_mixed_invalid;
}

} // namespace bioseq::pipeline_stats

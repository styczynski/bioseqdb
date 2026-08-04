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
import bioseq.highway.dispatch_contract;
import bioseq.indexing.minimizer_hashmap;
import bioseq.parallelism.memory_admission;
import bioseq.sequences.minimap_port;

export namespace bioseq::pipeline_stats {

/// Requested or actually executed biological compute backend.
enum class ExecutedBackend : std::uint8_t {
    none,
    scalar_cpu,
    simd_cpu,
    vulkan_gpu,
    mixed_invalid,
};

/// Proven origin of the target minimizer index consumed by a job.
enum class TargetIndexSource : std::uint8_t {
    none,
    recomputed,
    persisted,
    mixed_invalid,
};

/// Statement-stable identity for the requested and operation-observed SIMD
/// chaining policy. This deliberately has its own none/mixed sentinels rather
/// than overloading the algorithm enum: pipeline aggregation must distinguish
/// "no SIMD policy applies" from "two concrete policy identities were mixed".
enum class SimdChainingPolicyIdentity : std::uint8_t {
    none,
    exact_visited,
    bounded_batch_approximate,
    mixed_invalid,
};

/// Statement-stable identity for the SIMD base-alignment algorithm.  This is
/// intentionally narrower than BaseAlignmentAlgorithmType: the SIMD evidence
/// object only needs to distinguish an authentic Block Aligner execution from
/// absence or a mixed/forbidden alignment route.
enum class SimdAlignmentAlgorithmIdentity : std::uint8_t {
    none,
    block_aligner,
    mixed_invalid,
};

/// Stable public spelling used by PostgreSQL/report evidence. Keep this on the
/// aggregate identity rather than translating through the algorithm-local enum:
/// `none` and `mixed_invalid` are statement-level states that the local policy
/// type deliberately cannot represent.
[[nodiscard]] constexpr std::string_view simd_chaining_policy_identity_name(
    SimdChainingPolicyIdentity policy) noexcept
{
    switch (policy) {
        case SimdChainingPolicyIdentity::none:
            return "none";
        case SimdChainingPolicyIdentity::exact_visited:
            return "exact_visited";
        case SimdChainingPolicyIdentity::bounded_batch_approximate:
            return "bounded_batch_approximate";
        case SimdChainingPolicyIdentity::mixed_invalid:
            return "mixed_invalid";
    }
    return "mixed_invalid";
}

[[nodiscard]] constexpr std::string_view simd_alignment_algorithm_identity_name(
    SimdAlignmentAlgorithmIdentity algorithm) noexcept
{
    switch (algorithm) {
        case SimdAlignmentAlgorithmIdentity::none:
            return "none";
        case SimdAlignmentAlgorithmIdentity::block_aligner:
            return "block_aligner";
        case SimdAlignmentAlgorithmIdentity::mixed_invalid:
            return "mixed_invalid";
    }
    return "mixed_invalid";
}

/// Mutually exclusive engine credited with executing one pipeline stage.
enum class ExecutionEngine : std::uint8_t {
    host_io,
    scalar_cpu,
    simd_cpu,
    vulkan_gpu,
    count,
};

/// Stable SQL/report ordinal for every attested pipeline stage.
enum class PipelineStage : std::uint8_t {
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
inline constexpr std::size_t execution_engine_count =
    static_cast<std::size_t>(ExecutionEngine::count);
/// Number of stable pipeline-stage ordinals, excluding the count sentinel.
inline constexpr std::size_t pipeline_stage_count =
    static_cast<std::size_t>(PipelineStage::count);
/// Sentinel used when no failing target/query ordinal has been recorded.
inline constexpr std::uint64_t unset_job_ordinal =
    (std::numeric_limits<std::uint64_t>::max)();
/// Largest value representable by SQL bigint without lossy conversion.
inline constexpr std::uint64_t sql_bigint_value_max =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
/// Largest worker count representable by SQL integer.
inline constexpr std::uint32_t sql_integer_value_max =
    static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());

/// Return the stable report name for a biological backend value.
[[nodiscard]] constexpr std::string_view executed_backend_name(
    ExecutedBackend backend) noexcept {
    switch (backend) {
        case ExecutedBackend::none: return "none";
        case ExecutedBackend::scalar_cpu: return "scalar_cpu";
        case ExecutedBackend::simd_cpu: return "simd_cpu";
        case ExecutedBackend::vulkan_gpu: return "vulkan_gpu";
        case ExecutedBackend::mixed_invalid: return "mixed_invalid";
    }
    return "unknown";
}

/// Return the stable report name for a target-index source value.
[[nodiscard]] constexpr std::string_view target_index_source_name(
    TargetIndexSource source) noexcept {
    switch (source) {
        case TargetIndexSource::none: return "none";
        case TargetIndexSource::recomputed: return "recomputed";
        case TargetIndexSource::persisted: return "persisted";
        case TargetIndexSource::mixed_invalid: return "mixed_invalid";
    }
    return "unknown";
}

/// Return the stable report name for an execution engine value.
[[nodiscard]] constexpr std::string_view execution_engine_name(
    ExecutionEngine engine) noexcept {
    switch (engine) {
        case ExecutionEngine::host_io: return "host_io";
        case ExecutionEngine::scalar_cpu: return "scalar_cpu";
        case ExecutionEngine::simd_cpu: return "simd_cpu";
        case ExecutionEngine::vulkan_gpu: return "vulkan_gpu";
        case ExecutionEngine::count: return "count";
    }
    return "unknown";
}

/// Return the stable report/SRF name for a pipeline-stage ordinal.
[[nodiscard]] constexpr std::string_view pipeline_stage_name(
    PipelineStage stage) noexcept {
    switch (stage) {
        case PipelineStage::query_sketch: return "query_sketch";
        case PipelineStage::target_sketch: return "target_sketch";
        case PipelineStage::target_index_build: return "target_index_build";
        case PipelineStage::persisted_index_decode: return "persisted_index_decode";
        case PipelineStage::partition_selection: return "partition_selection";
        case PipelineStage::device_index_upload: return "device_index_upload";
        case PipelineStage::seed_lookup: return "seed_lookup";
        case PipelineStage::chain_score: return "chain_score";
        case PipelineStage::chain_backtrack: return "chain_backtrack";
        case PipelineStage::region_generation: return "region_generation";
        case PipelineStage::primary_secondary_reduction: return "primary_secondary_reduction";
        case PipelineStage::divergence_estimation: return "divergence_estimation";
        case PipelineStage::mapq: return "mapq";
        case PipelineStage::target_window_plan: return "target_window_plan";
        case PipelineStage::target_window_io: return "target_window_io";
        case PipelineStage::target_window_upload: return "target_window_upload";
        case PipelineStage::cigar_job_generation: return "cigar_job_generation";
        case PipelineStage::left_extension: return "left_extension";
        case PipelineStage::gap_extension: return "gap_extension";
        case PipelineStage::right_extension: return "right_extension";
        case PipelineStage::count: return "count";
    }
    return "unknown";
}

/// Convert a validated engine enum to its coverage-array index.
[[nodiscard]] constexpr std::size_t engine_index(ExecutionEngine engine) noexcept {
    return static_cast<std::size_t>(engine);
}

/// Convert a validated stage enum to its coverage-array index.
[[nodiscard]] constexpr std::size_t stage_index(PipelineStage stage) noexcept {
    return static_cast<std::size_t>(stage);
}

/// Merge a SQL-bound count/time/byte value with the project's shared
/// saturating arithmetic. PostgreSQL bigint is signed, so values clamp at
/// INT64_MAX; an exact sum of INT64_MAX is representable and is not saturation.
[[nodiscard]] inline bool merge_counter(
    std::uint64_t& destination,
    std::uint64_t increment) noexcept {
    if (destination > sql_bigint_value_max) {
        destination = sql_bigint_value_max;
        return true;
    }
    if (increment > sql_bigint_value_max - destination) {
        destination = sql_bigint_value_max;
        return true;
    }
    destination = bioseq::parallelism::saturating_add(destination, increment);
    return false;
}

/// Target-independent projection of the operation-local Block Aligner proof.
/// Every field is copied at the dispatch boundary from
/// BlockAlignerExecutionProof; stage coverage and generic SIMD counters are
/// never used to manufacture this evidence.
struct BlockAlignerExecutionEvidence {
    bool present = false;
    bool accepted = false;
    std::uint64_t executed_calls = 0;
    std::uint64_t nonempty_calls = 0;
    std::uint64_t nonempty_calls_with_simd = 0;
    std::uint64_t simd_dp_vector_batches = 0;
    std::uint64_t simd_dp_cells = 0;
    std::uint64_t forbidden_ksw2_calls = 0;
    std::uint64_t forbidden_scalar_biological_fallbacks = 0;
    bool saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const BlockAlignerExecutionEvidence&) const noexcept = default;

    /// Merge independently captured operation proofs.  `accepted` means every
    /// present operand was accepted; SQL-bound counter overflow revokes it.
    [[nodiscard]] bool merge(
        const BlockAlignerExecutionEvidence& rhs) noexcept {
        if (!rhs.present) {
            const bool malformed_absence = rhs.accepted
                || rhs.executed_calls != 0
                || rhs.nonempty_calls != 0
                || rhs.nonempty_calls_with_simd != 0
                || rhs.simd_dp_vector_batches != 0
                || rhs.simd_dp_cells != 0
                || rhs.forbidden_ksw2_calls != 0
                || rhs.forbidden_scalar_biological_fallbacks != 0
                || rhs.saturated;
            if (malformed_absence) {
                saturated = true;
                accepted = false;
            }
            return saturated;
        }

        if (!present) {
            present = true;
            accepted = rhs.accepted;
        } else {
            accepted = accepted && rhs.accepted;
        }
        bool overflow = saturated || rhs.saturated;
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
        saturated = overflow;
        if (saturated) accepted = false;
        return saturated;
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
struct StageCoverage {
    std::uint64_t eligible_jobs = 0;
    std::array<std::uint64_t, execution_engine_count> executed_jobs{};
    std::uint64_t fallback_jobs = 0;

    [[nodiscard]] constexpr bool operator==(
        const StageCoverage&) const noexcept = default;

    /// Saturating merge; true means at least one SQL-bound count overflowed.
    [[nodiscard]] bool merge(const StageCoverage& rhs) noexcept {
        bool saturated = merge_counter(eligible_jobs, rhs.eligible_jobs);
        for (std::size_t i = 0; i < executed_jobs.size(); ++i) {
            saturated = merge_counter(executed_jobs[i], rhs.executed_jobs[i]) || saturated;
        }
        saturated = merge_counter(fallback_jobs, rhs.fallback_jobs) || saturated;
        return saturated;
    }
};

/// Coverage for every stable pipeline-stage ordinal.
struct PipelineJobStageCoverage {
    std::array<StageCoverage, pipeline_stage_count> coverage{};

    [[nodiscard]] constexpr bool operator==(
        const PipelineJobStageCoverage&) const noexcept = default;

    /// Merge all stage coverages and report whether any count saturated.
    [[nodiscard]] bool merge(const PipelineJobStageCoverage& rhs) noexcept {
        bool saturated = false;
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            saturated = coverage[i].merge(rhs.coverage[i]) || saturated;
        }
        return saturated;
    }
};

/// Host and device time attributed to one pipeline stage.
struct PipelineStageTiming {
    std::uint64_t host_wall_ns = 0;
    std::uint64_t gpu_device_ns = 0;

    [[nodiscard]] constexpr bool operator==(
        const PipelineStageTiming&) const noexcept = default;
};

/// Merge a lossless u64 identity that may be published exactly once.
inline void merge_report_once_identity(
    std::uint64_t& destination,
    bool& destination_present,
    std::uint64_t incoming,
    bool incoming_present,
    bool& identity_conflict) noexcept {
    if ((!destination_present && destination != 0)
        || (!incoming_present && incoming != 0)) {
        identity_conflict = true;
    }
    if (!incoming_present) return;
    if (!destination_present) {
        destination = incoming;
        destination_present = true;
        return;
    }
    identity_conflict = true;
}

/// Exact Format-6 cold-reader receipt kept out of the frozen alignment-stats
/// column ABI. Identity fields are merged only when identical; counters use
/// the same saturating arithmetic as the rest of the operational snapshot.
struct Format6ReaderOperationalEvidence {
    std::uint64_t format_version = 0;
    std::uint64_t sessions = 0;
    bool identity_present = false;
    std::uint64_t generation_id = 0;
    std::array<std::byte, 16> source_identity{};
    std::uint64_t source_content_crc64 = 0;
    std::array<std::byte, 32> compatibility_digest{};
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
    std::uint64_t directory_phase_peak_bytes = 0;
    std::uint64_t control_phase_peak_bytes = 0;
    std::uint64_t posting_phase_peak_bytes = 0;
    std::uint64_t peak_admitted_bytes = 0;
    bool owner_bytes_present = false;
    std::uint64_t retained_owner_bytes = 0;
    std::uint64_t admitted_bytes = 0;
    std::uint64_t generic_source_fallback_calls = 0;
    std::uint64_t failures = 0;
    bool saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const Format6ReaderOperationalEvidence&) const noexcept = default;

    [[nodiscard]] bool merge(
            const Format6ReaderOperationalEvidence& rhs,
            bool& identity_conflict) noexcept {
        bool overflow = saturated || rhs.saturated;
        const auto add = [&overflow](
                std::uint64_t& destination,
                const std::uint64_t increment) noexcept {
            overflow = merge_counter(destination, increment) || overflow;
        };
        if (rhs.format_version != 0u) {
            if (format_version == 0u) {
                format_version = rhs.format_version;
            } else if (format_version != rhs.format_version) {
                identity_conflict = true;
            }
        }
        const bool destination_identity_payload =
            generation_id != 0u
            || source_identity != std::array<std::byte, 16>{}
            || source_content_crc64 != 0u
            || compatibility_digest != std::array<std::byte, 32>{};
        const bool incoming_identity_payload =
            rhs.generation_id != 0u
            || rhs.source_identity != std::array<std::byte, 16>{}
            || rhs.source_content_crc64 != 0u
            || rhs.compatibility_digest != std::array<std::byte, 32>{};
        if ((!identity_present && destination_identity_payload)
            || (!rhs.identity_present && incoming_identity_payload)) {
            identity_conflict = true;
        }
        if (rhs.identity_present) {
            if (!identity_present) {
                identity_present = true;
                generation_id = rhs.generation_id;
                source_identity = rhs.source_identity;
                source_content_crc64 = rhs.source_content_crc64;
                compatibility_digest = rhs.compatibility_digest;
            } else if (generation_id != rhs.generation_id
                || source_identity != rhs.source_identity
                || source_content_crc64 != rhs.source_content_crc64
                || compatibility_digest != rhs.compatibility_digest) {
                identity_conflict = true;
            }
        }
        const bool destination_owner_payload =
            retained_owner_bytes != 0u || admitted_bytes != 0u;
        const bool incoming_owner_payload =
            rhs.retained_owner_bytes != 0u || rhs.admitted_bytes != 0u;
        if ((!owner_bytes_present && destination_owner_payload)
            || (!rhs.owner_bytes_present && incoming_owner_payload)) {
            identity_conflict = true;
        }
        if (rhs.owner_bytes_present) {
            if (!owner_bytes_present) {
                owner_bytes_present = true;
                retained_owner_bytes = rhs.retained_owner_bytes;
                admitted_bytes = rhs.admitted_bytes;
            } else if (retained_owner_bytes != rhs.retained_owner_bytes
                || admitted_bytes != rhs.admitted_bytes) {
                identity_conflict = true;
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
        directory_phase_peak_bytes = std::max(
            directory_phase_peak_bytes,
            rhs.directory_phase_peak_bytes);
        control_phase_peak_bytes = std::max(
            control_phase_peak_bytes,
            rhs.control_phase_peak_bytes);
        posting_phase_peak_bytes = std::max(
            posting_phase_peak_bytes,
            rhs.posting_phase_peak_bytes);
        peak_admitted_bytes = std::max(
            peak_admitted_bytes,
            rhs.peak_admitted_bytes);
        add(generic_source_fallback_calls,
            rhs.generic_source_fallback_calls);
        add(failures, rhs.failures);
        saturated = overflow;
        return overflow;
    }
};

/// Stable result code shared by Format-5 mapped-sink preallocation and
/// mapping-advice receipts. These values are report ABI, not raw errno values.
enum class Format5EvidenceOperationResult : std::uint64_t {
    not_attempted = 0u,
    succeeded = 1u,
    failed = 2u,
};

/// Stable identity of the selected lazy extent-validation state layout.
enum class Format5LazyStateLayout : std::uint64_t {
    unselected = 0u,
    compact_aligned_planes = 1u,
    cache_line_padded_entries = 2u,
};

/// Stable identity of the selected read-only mapping advice policy.
enum class Format5MappingAdvice : std::uint64_t {
    none = 0u,
    random = 1u,
};

static_assert(static_cast<std::uint64_t>(
    Format5EvidenceOperationResult::not_attempted) == 0u);
static_assert(static_cast<std::uint64_t>(
    Format5EvidenceOperationResult::succeeded) == 1u);
static_assert(static_cast<std::uint64_t>(
    Format5EvidenceOperationResult::failed) == 2u);
static_assert(static_cast<std::uint64_t>(
    Format5LazyStateLayout::unselected) == 0u);
static_assert(static_cast<std::uint64_t>(
    Format5LazyStateLayout::compact_aligned_planes) == 1u);
static_assert(static_cast<std::uint64_t>(
    Format5LazyStateLayout::cache_line_padded_entries) == 2u);
static_assert(static_cast<std::uint64_t>(
    Format5MappingAdvice::none) == 0u);
static_assert(static_cast<std::uint64_t>(
    Format5MappingAdvice::random) == 1u);

/// Provider, mapped-image and lifecycle evidence appended by the exact
/// Format-5 evidence V4 schema. Counter producers build operation-local
/// deltas through record_counter(); aggregation remains checked and clamps at
/// SQL bigint max. Stable codes use explicit presence bits because provider
/// code zero (postgres_relation) is a valid published identity.
struct Format5ProviderOperationalEvidence {
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
    std::uint64_t lazy_extent_invalid_receipts = 0u;
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
    bool provider_configured_present = false;
    bool provider_captured_present = false;
    bool provider_persisted_present = false;
    bool provider_opened_present = false;
    bool mapped_sink_preallocation_result_present = false;
    bool lazy_state_layout_present = false;
    bool madvise_choice_present = false;
    bool madvise_result_present = false;
    bool stable_value_conflict = false;
    bool saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const Format5ProviderOperationalEvidence&) const noexcept = default;

    /// Add one SQL-bound count/byte receipt and retain a local saturation bit.
    void record_counter(
            std::uint64_t& destination,
            std::uint64_t increment) noexcept {
        saturated = merge_counter(destination, increment) || saturated;
    }

    /// File-action occupancy is a high-water mark, not an additive count.
    void record_file_action_high_water(std::uint64_t value) noexcept {
        if (value > sql_bigint_value_max) {
            value = sql_bigint_value_max;
            saturated = true;
        }
        file_action_high_water = std::max(file_action_high_water, value);
    }

    void record_provider_configured(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_configured,
            provider_configured_present,
            provider,
            stable_value_conflict);
    }

    void record_provider_captured(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_captured,
            provider_captured_present,
            provider,
            stable_value_conflict);
    }

    void record_provider_persisted(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_persisted,
            provider_persisted_present,
            provider,
            stable_value_conflict);
    }

    void record_provider_opened(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_opened,
            provider_opened_present,
            provider,
            stable_value_conflict);
    }

    void record_mapped_sink_preallocation_result(
            Format5EvidenceOperationResult result) noexcept {
        record_stable_value(
            mapped_sink_preallocation_result,
            mapped_sink_preallocation_result_present,
            static_cast<std::uint64_t>(result),
            stable_value_conflict);
    }

    void record_lazy_state_layout(Format5LazyStateLayout layout) noexcept {
        record_stable_value(
            lazy_state_layout,
            lazy_state_layout_present,
            static_cast<std::uint64_t>(layout),
            stable_value_conflict);
    }

    void record_madvise(
            Format5MappingAdvice choice,
            Format5EvidenceOperationResult result) noexcept {
        record_stable_value(
            madvise_choice,
            madvise_choice_present,
            static_cast<std::uint64_t>(choice),
            stable_value_conflict);
        record_stable_value(
            madvise_result,
            madvise_result_present,
            static_cast<std::uint64_t>(result),
            stable_value_conflict);
    }

    [[nodiscard]] constexpr bool
    provider_identity_complete_and_consistent() const noexcept {
        return provider_configured_present
            && provider_captured_present
            && provider_persisted_present
            && provider_opened_present
            && !stable_value_conflict
            && provider_configured == provider_captured
            && provider_configured == provider_persisted
            && provider_configured == provider_opened;
    }

    /// Merge one operation-local provider/lifecycle delta.
    [[nodiscard]] bool merge(
            const Format5ProviderOperationalEvidence& rhs,
            bool& identity_conflict) noexcept {
        bool overflow = saturated || rhs.saturated;
        bool conflict = stable_value_conflict || rhs.stable_value_conflict;
        merge_stable_value(
            provider_configured, provider_configured_present,
            rhs.provider_configured, rhs.provider_configured_present,
            conflict);
        merge_stable_value(
            provider_captured, provider_captured_present,
            rhs.provider_captured, rhs.provider_captured_present,
            conflict);
        merge_stable_value(
            provider_persisted, provider_persisted_present,
            rhs.provider_persisted, rhs.provider_persisted_present,
            conflict);
        merge_stable_value(
            provider_opened, provider_opened_present,
            rhs.provider_opened, rhs.provider_opened_present,
            conflict);
        merge_stable_value(
            mapped_sink_preallocation_result,
            mapped_sink_preallocation_result_present,
            rhs.mapped_sink_preallocation_result,
            rhs.mapped_sink_preallocation_result_present,
            conflict);
        merge_stable_value(
            lazy_state_layout, lazy_state_layout_present,
            rhs.lazy_state_layout, rhs.lazy_state_layout_present,
            conflict);
        merge_stable_value(
            madvise_choice, madvise_choice_present,
            rhs.madvise_choice, rhs.madvise_choice_present,
            conflict);
        merge_stable_value(
            madvise_result, madvise_result_present,
            rhs.madvise_result, rhs.madvise_result_present,
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
        add(lazy_extent_invalid_receipts,
            rhs.lazy_extent_invalid_receipts);
        add(mapping_bytes, rhs.mapping_bytes);
        add(mapping_count, rhs.mapping_count);
        add(mapping_resourceowner_releases,
            rhs.mapping_resourceowner_releases);
        const auto bounded_file_action_high_water =
            std::min(rhs.file_action_high_water, sql_bigint_value_max);
        overflow = overflow || rhs.file_action_high_water > sql_bigint_value_max;
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

        saturated = overflow;
        stable_value_conflict = conflict;
        identity_conflict = identity_conflict || conflict;
        return overflow;
    }

private:
    static void record_stable_value(
            std::uint64_t& destination,
            bool& destination_present,
            std::uint64_t incoming,
            bool& conflict) noexcept {
        if (!destination_present) {
            destination = incoming;
            destination_present = true;
            return;
        }
        if (destination != incoming) conflict = true;
    }

    static void merge_stable_value(
            std::uint64_t& destination,
            bool& destination_present,
            std::uint64_t incoming,
            bool incoming_present,
            bool& conflict) noexcept {
        if ((!destination_present && destination != 0u)
            || (!incoming_present && incoming != 0u)) {
            conflict = true;
        }
        if (!incoming_present) return;
        record_stable_value(
            destination,
            destination_present,
            incoming,
            conflict);
    }
};

/// Once-per-buffer operational counters and report-once identities.
struct PipelineOperationalStats {
    std::array<PipelineStageTiming, pipeline_stage_count> timing{};
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
    // serializers emit NULL when *_present is false and lossless u64 text when
    // true (CRC zero is valid and is distinguished by the presence bit).
    std::uint64_t cpu_prepared_key_set_checksum = 0;
    bool cpu_prepared_key_set_checksum_present = false;
    std::uint64_t cpu_prepared_build_set_checksum = 0;
    bool cpu_prepared_build_set_checksum_present = false;
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
    std::uint64_t format5_reader_capacity_accounting_passes = 0;
    std::uint64_t format5_reader_global_position_storage_preparations = 0;
    std::uint64_t format5_reader_global_position_storage_allocations = 0;
    std::uint64_t format5_reader_position_append_growth_reservations = 0;
    std::uint64_t format5_reader_retained_position_count = 0;
    std::uint64_t format5_reader_retained_position_capacity = 0;
    std::uint64_t format5_reader_directory_capacity_bytes = 0;
    std::uint64_t format5_reader_query_hash_capacity_bytes = 0;
    std::uint64_t format5_reader_query_record_capacity_bytes = 0;
    std::uint64_t format5_reader_ordinal_route_capacity_bytes = 0;
    std::uint64_t format5_reader_selected_extent_capacity_bytes = 0;
    std::uint64_t format5_reader_current_selected_control_capacity_bytes = 0;
    std::uint64_t format5_reader_global_position_capacity_bytes = 0;
    std::uint64_t format5_reader_control_payload_capacity_bytes = 0;
    std::uint64_t format5_reader_owned_capacity_bytes = 0;
    std::uint64_t format5_reader_admitted_bytes = 0;
    std::uint64_t format5_reader_exact_position_image_checks = 0;
    std::uint64_t format5_reader_exact_position_image_failures = 0;
    std::uint64_t format5_reader_retained_layout_checks = 0;
    std::uint64_t format5_reader_retained_layout_failures = 0;
    bool format5_reader_memory_evidence_saturated = false;
    Format5ProviderOperationalEvidence format5_provider_evidence{};
    bioseq::indexing::PreparedFragmentAnchorBatchEvidence
        fragment_native_query_evidence{};
    Format6ReaderOperationalEvidence format6_reader_evidence{};
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
    bool simd_chaining_policy_evidence_saturated = false;
    BlockAlignerExecutionEvidence block_aligner_execution_evidence{};
    ::bioseq::prefilter_stats::PrefilterExecutionProof
        prefilter_execution_proof{};
    bool prefilter_execution_proof_present = false;
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
    // or predecessor backtracking and must never satisfy either proof counter.
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
	// Cold grouped32 source/route proof. The two legacy counters name only
	// pair-index conversion work; segment-local32 validation inside the direct
	// reader is not a legacy globalize/rebase pass.
	std::uint64_t persisted_source_parent_rows_read = 0;
	std::uint64_t persisted_source_catalog_rows_read = 0;
	std::uint64_t persisted_source_catalog_index_scans = 0;
	std::uint64_t persisted_source_fragment_rows_read = 0;
	std::uint64_t persisted_source_decoded_bp = 0;
	std::uint64_t persisted_legacy_globalize_calls = 0;
	std::uint64_t persisted_legacy_rebase_calls = 0;
    // Runtime-only proof that the selected fixed-target grouped32 validator
    // was actually entered.  Plan construction cannot know the persisted wire
    // format selected for a row, so this counter is populated only from the
    // completed PreparedGrouped32QueryReader route evidence.
    std::uint64_t indexing_grouped32_validate_calls = 0;
    // PostgreSQL cold persisted-index preparation. These are execution
    // diagnostics only; backend/fallback acceptance remains owned by the
    // existing requested/actual stage proof above.
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
    bool operational_identity_conflict = false;
    bool stats_saturated = false;

    // Defaulted comparison is deliberately member-wise. It includes every
    // timing, counter, identity value/presence bit, and failure flag without
    // observing object padding (memcmp is forbidden for proof values).
    [[nodiscard]] constexpr bool operator==(
        const PipelineOperationalStats&) const noexcept = default;

    /// Authentic vector activity may originate from the continuation kernel
    /// or the whole-range resident vector_stream kernel. The two counts remain
    /// separately interpretable above.
    [[nodiscard]] constexpr bool has_nthash_vector_activity() const noexcept {
        return nthash_vector_batches != 0
            || nthash_resident_vector_stream_kernel_calls != 0;
    }

    /// Merge one completed operation-local prefilter proof. The authoritative
    /// proof type owns all counter arithmetic and invariants; the statement
    /// aggregate only retains its presence and propagates invalidity into the
    /// existing fail-closed operational saturation signal.
    void record_prefilter_execution_proof(
            const ::bioseq::prefilter_stats::PrefilterExecutionProof&
                proof) noexcept {
        prefilter_execution_proof.combine(proof);
        prefilter_execution_proof_present = true;
        if (!prefilter_execution_proof.valid()) {
            prefilter_execution_proof.saturated = true;
            stats_saturated = true;
        }
    }

    /// Merge one operational delta without multiplying it by logical jobs.
    void merge(const PipelineOperationalStats& rhs) noexcept {
        using PrefilterProof =
            ::bioseq::prefilter_stats::PrefilterExecutionProof;
        const PrefilterProof empty_prefilter_proof{};
        if (prefilter_execution_proof_present
            && !prefilter_execution_proof.valid()) {
            prefilter_execution_proof.saturated = true;
            stats_saturated = true;
        } else if (!prefilter_execution_proof_present
                   && prefilter_execution_proof != empty_prefilter_proof) {
            prefilter_execution_proof.saturated = true;
            prefilter_execution_proof_present = true;
            stats_saturated = true;
        }
        if (rhs.prefilter_execution_proof_present) {
            record_prefilter_execution_proof(
                rhs.prefilter_execution_proof);
        } else if (rhs.prefilter_execution_proof
                   != empty_prefilter_proof) {
            PrefilterProof malformed = rhs.prefilter_execution_proof;
            malformed.saturated = true;
            record_prefilter_execution_proof(malformed);
        }
        bool saturated = stats_saturated || rhs.stats_saturated;
        operational_identity_conflict = operational_identity_conflict
            || rhs.operational_identity_conflict;
        merge_report_once_identity(
            cpu_prepared_key_set_checksum,
            cpu_prepared_key_set_checksum_present,
            rhs.cpu_prepared_key_set_checksum,
            rhs.cpu_prepared_key_set_checksum_present,
            operational_identity_conflict);
        merge_report_once_identity(
            cpu_prepared_build_set_checksum,
            cpu_prepared_build_set_checksum_present,
            rhs.cpu_prepared_build_set_checksum,
            rhs.cpu_prepared_build_set_checksum_present,
            operational_identity_conflict);
        auto add = [&saturated](
            std::uint64_t& destination,
            std::uint64_t increment) noexcept {
            saturated = merge_counter(destination, increment) || saturated;
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
        add(format5_reader_capacity_accounting_passes,
            rhs.format5_reader_capacity_accounting_passes);
        add(format5_reader_global_position_storage_preparations,
            rhs.format5_reader_global_position_storage_preparations);
        add(format5_reader_global_position_storage_allocations,
            rhs.format5_reader_global_position_storage_allocations);
        add(format5_reader_position_append_growth_reservations,
            rhs.format5_reader_position_append_growth_reservations);
        add(format5_reader_retained_position_count,
            rhs.format5_reader_retained_position_count);
        add(format5_reader_retained_position_capacity,
            rhs.format5_reader_retained_position_capacity);
        add(format5_reader_directory_capacity_bytes,
            rhs.format5_reader_directory_capacity_bytes);
        add(format5_reader_query_hash_capacity_bytes,
            rhs.format5_reader_query_hash_capacity_bytes);
        add(format5_reader_query_record_capacity_bytes,
            rhs.format5_reader_query_record_capacity_bytes);
        add(format5_reader_ordinal_route_capacity_bytes,
            rhs.format5_reader_ordinal_route_capacity_bytes);
        add(format5_reader_selected_extent_capacity_bytes,
            rhs.format5_reader_selected_extent_capacity_bytes);
        add(format5_reader_current_selected_control_capacity_bytes,
            rhs.format5_reader_current_selected_control_capacity_bytes);
        add(format5_reader_global_position_capacity_bytes,
            rhs.format5_reader_global_position_capacity_bytes);
        add(format5_reader_control_payload_capacity_bytes,
            rhs.format5_reader_control_payload_capacity_bytes);
        add(format5_reader_owned_capacity_bytes,
            rhs.format5_reader_owned_capacity_bytes);
        add(format5_reader_admitted_bytes,
            rhs.format5_reader_admitted_bytes);
        add(format5_reader_exact_position_image_checks,
            rhs.format5_reader_exact_position_image_checks);
        add(format5_reader_exact_position_image_failures,
            rhs.format5_reader_exact_position_image_failures);
        add(format5_reader_retained_layout_checks,
            rhs.format5_reader_retained_layout_checks);
        add(format5_reader_retained_layout_failures,
            rhs.format5_reader_retained_layout_failures);
        format5_reader_memory_evidence_saturated =
            format5_reader_memory_evidence_saturated
            || rhs.format5_reader_memory_evidence_saturated;
        saturated = format5_provider_evidence.merge(
            rhs.format5_provider_evidence,
            operational_identity_conflict) || saturated;
        format5_reader_memory_evidence_saturated =
            format5_reader_memory_evidence_saturated
            || format5_provider_evidence.saturated;
        fragment_native_query_evidence.merge(
            rhs.fragment_native_query_evidence);
        saturated = fragment_native_query_evidence.saturated || saturated;
        saturated = format6_reader_evidence.merge(
            rhs.format6_reader_evidence,
            operational_identity_conflict) || saturated;
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
        simd_chaining_policy_evidence_saturated =
            simd_chaining_policy_evidence_saturated
            || rhs.simd_chaining_policy_evidence_saturated;
        saturated = block_aligner_execution_evidence.merge(
            rhs.block_aligner_execution_evidence) || saturated;
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
		add(persisted_legacy_globalize_calls,
			rhs.persisted_legacy_globalize_calls);
		add(persisted_legacy_rebase_calls,
			rhs.persisted_legacy_rebase_calls);
        add(indexing_grouped32_validate_calls,
            rhs.indexing_grouped32_validate_calls);
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
        stats_saturated = saturated;
    }
};

/// Tri-state optional proof bit plus a fail-closed mixed sentinel.
enum class OptionalProofBool : std::uint8_t {
    not_applicable,
    false_value,
    true_value,
    mixed_invalid,
};

/// Pipeline-cache preparation policy proven by the timed GPU runtime.
enum class PipelineCachePolicy : std::uint8_t {
    not_applicable,
    immutable_preseed_all_hit,
    isolated_miss_diagnostic,
    mixed_invalid,
};

/// Return the stable SQL/report spelling of an optional proof bit.
[[nodiscard]] constexpr std::string_view optional_proof_bool_name(
    OptionalProofBool value) noexcept {
    switch (value) {
        case OptionalProofBool::not_applicable: return "not_applicable";
        case OptionalProofBool::false_value: return "false";
        case OptionalProofBool::true_value: return "true";
        case OptionalProofBool::mixed_invalid: return "mixed_invalid";
    }
    return "unknown";
}

/// Return the stable SQL/report spelling of a pipeline-cache policy.
[[nodiscard]] constexpr std::string_view pipeline_cache_policy_name(
    PipelineCachePolicy policy) noexcept {
    switch (policy) {
        case PipelineCachePolicy::not_applicable: return "not_applicable";
        case PipelineCachePolicy::immutable_preseed_all_hit:
            return "immutable_preseed_all_hit";
        case PipelineCachePolicy::isolated_miss_diagnostic:
            return "isolated_miss_diagnostic";
        case PipelineCachePolicy::mixed_invalid: return "mixed_invalid";
    }
    return "unknown";
}

/// Return whether every byte/element of a fixed proof field is zero.
template <typename T, std::size_t N>
[[nodiscard]] constexpr bool all_zero(const std::array<T, N>& value) noexcept {
    for (const T element : value) {
        if (element != T{}) return false;
    }
    return true;
}

/// Exact CPU/GPU execution environment captured by the timed runtime.
struct ExecutionEnvironmentProof {
    std::uint32_t effective_cpu_workers = 0;
    std::int64_t highway_compiled_targets = 0;
    std::int64_t highway_cpu_supported_targets = 0;
    std::int64_t highway_runnable_targets = 0;
    std::int64_t highway_executed_target = 0;
    bool highway_executed_target_mixed = false;
    bool gpu_environment_present = false;
    std::array<std::uint8_t, 16> gpu_device_uuid{};
    std::array<char, 128> gpu_driver_identity{};
    PipelineCachePolicy gpu_pipeline_cache_policy = PipelineCachePolicy::not_applicable;
    std::array<std::uint8_t, 32> gpu_pipeline_cache_seed_sha256{};
    std::uint64_t gpu_pipeline_cache_seed_size = 0;
    std::uint64_t gpu_pipeline_cache_manifest_checksum = 0;
    OptionalProofBool gpu_pipeline_cache_hit = OptionalProofBool::not_applicable;
    bool mixed_invalid = false;

    /// True only for the canonical never-captured environment sentinel.
    [[nodiscard]] static constexpr bool is_unset(
        const ExecutionEnvironmentProof& value) noexcept {
        return value.effective_cpu_workers == 0
            && value.highway_compiled_targets == 0
            && value.highway_cpu_supported_targets == 0
            && value.highway_runnable_targets == 0
            && value.highway_executed_target == 0
            && !value.highway_executed_target_mixed
            && !value.gpu_environment_present
            && all_zero(value.gpu_device_uuid)
            && all_zero(value.gpu_driver_identity)
            && value.gpu_pipeline_cache_policy
                == PipelineCachePolicy::not_applicable
            && all_zero(value.gpu_pipeline_cache_seed_sha256)
            && value.gpu_pipeline_cache_seed_size == 0
            && value.gpu_pipeline_cache_manifest_checksum == 0
            && value.gpu_pipeline_cache_hit
                == OptionalProofBool::not_applicable
            && !value.mixed_invalid;
    }

    /// Compare every environment identity field except mixed-invalid state.
    [[nodiscard]] static constexpr bool payload_equal(
        const ExecutionEnvironmentProof& lhs,
        const ExecutionEnvironmentProof& rhs) noexcept {
        return non_highway_payload_equal(lhs, rhs)
            && lhs.highway_compiled_targets
                == rhs.highway_compiled_targets
            && lhs.highway_cpu_supported_targets
                == rhs.highway_cpu_supported_targets
            && lhs.highway_runnable_targets
                == rhs.highway_runnable_targets;
    }

    /// Compare environment identity while treating a zero Highway mask as an
    /// observation that has not arrived yet.  This is the compatibility
    /// predicate used only while merging independently completed proof shards;
    /// final validation still requires an all-or-none, internally consistent
    /// Highway plan and a singular executed target.
    [[nodiscard]] static constexpr bool payload_compatible(
        const ExecutionEnvironmentProof& lhs,
        const ExecutionEnvironmentProof& rhs) noexcept {
        const auto mask_compatible = [](const std::int64_t left,
                                        const std::int64_t right) noexcept {
            return left == 0 || right == 0 || left == right;
        };
        return non_highway_payload_equal(lhs, rhs)
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
    /// but a partially populated Highway proof is never accepted.
    [[nodiscard]] static constexpr bool highway_fields_are_absent(
        const ExecutionEnvironmentProof& value) noexcept {
        return value.highway_compiled_targets == 0
            && value.highway_cpu_supported_targets == 0
            && value.highway_runnable_targets == 0
            && value.highway_executed_target == 0
            && !value.highway_executed_target_mixed;
    }

    /// Validate the exact plan-mask relation and the binding returned by the
    /// sole dynamic-dispatch root.  Once any Highway field is present, absence,
    /// a multi-bit target, or a target outside the runnable set fails closed.
    [[nodiscard]] static constexpr bool highway_fields_are_valid(
        const ExecutionEnvironmentProof& value) noexcept {
        if (highway_fields_are_absent(value)) return true;
        if (value.highway_executed_target_mixed
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
    [[nodiscard]] static constexpr bool non_highway_payload_equal(
        const ExecutionEnvironmentProof& lhs,
        const ExecutionEnvironmentProof& rhs) noexcept {
        return lhs.effective_cpu_workers == rhs.effective_cpu_workers
            && lhs.gpu_environment_present == rhs.gpu_environment_present
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

    /// Adopt the first complete proof and fail closed on absence or drift.
    void merge(const ExecutionEnvironmentProof& rhs) noexcept {
        if (is_unset(*this)) {
            *this = rhs;
            if (rhs.effective_cpu_workers == 0 || rhs.mixed_invalid) {
                mixed_invalid = true;
            }
            return;
        }
        if (is_unset(rhs)
            || effective_cpu_workers == 0
            || rhs.effective_cpu_workers == 0
            || mixed_invalid
            || rhs.mixed_invalid
            || !payload_compatible(*this, rhs)) {
            mixed_invalid = true;
            return;
        }

        const auto merge_highway_mask = [this](
                std::int64_t& destination,
                const std::int64_t observed) noexcept {
            if (observed == 0) return;
            if (destination == 0) {
                destination = observed;
            } else if (destination != observed) {
                mixed_invalid = true;
            }
        };
        merge_highway_mask(
            highway_compiled_targets, rhs.highway_compiled_targets);
        merge_highway_mask(
            highway_cpu_supported_targets,
            rhs.highway_cpu_supported_targets);
        merge_highway_mask(
            highway_runnable_targets, rhs.highway_runnable_targets);

        if (highway_executed_target_mixed
            || rhs.highway_executed_target_mixed) {
            highway_executed_target_mixed = true;
            highway_executed_target = 0;
        } else if (highway_executed_target == 0) {
            highway_executed_target = rhs.highway_executed_target;
        } else if (rhs.highway_executed_target != 0
            && highway_executed_target
                != rhs.highway_executed_target) {
            highway_executed_target_mixed = true;
            highway_executed_target = 0;
        }
    }

    /// Record the registry plan and the binding returned by the sole runtime
    /// dispatch. Plan-mask drift is an environment invariant failure; target
    /// drift is represented explicitly so aggregation never reports a false
    /// singular architecture.
    void record_highway_target(
        const ::bioseq::simd_dispatch::HighwayDispatchInfo& plan,
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
            mixed_invalid = true;
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
            mixed_invalid = true;
            return;
        }
        if (highway_executed_target_mixed) {
            return;
        }
        if (highway_executed_target == 0) {
            highway_executed_target = executed_target;
        } else if (highway_executed_target != executed_target) {
            highway_executed_target = 0;
            highway_executed_target_mixed = true;
        }
    }
};

/// Complete attestation retained until one logical job is validated.
struct PipelineJobExecutionStats {
    ExecutedBackend requested_backend = ExecutedBackend::none;
    ExecutedBackend actual_backend = ExecutedBackend::none;
    SimdChainingPolicyIdentity requested_simd_chaining_policy =
        SimdChainingPolicyIdentity::none;
    SimdChainingPolicyIdentity observed_simd_chaining_policy =
        SimdChainingPolicyIdentity::none;
    SimdAlignmentAlgorithmIdentity requested_simd_alignment_algorithm =
        SimdAlignmentAlgorithmIdentity::none;
    SimdAlignmentAlgorithmIdentity observed_simd_alignment_algorithm =
        SimdAlignmentAlgorithmIdentity::none;
    TargetIndexSource requested_target_index_source = TargetIndexSource::none;
    TargetIndexSource actual_target_index_source = TargetIndexSource::none;
    std::uint64_t persisted_generation_id = 0;
    std::uint64_t persisted_source_identity = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t query_ordinal = 0;
    ExecutionEnvironmentProof environment{};
    PipelineJobStageCoverage stages{};
};

/// True only for an executable, non-sentinel biological backend.
[[nodiscard]] constexpr bool is_concrete_backend(ExecutedBackend backend) noexcept {
    return backend == ExecutedBackend::scalar_cpu
        || backend == ExecutedBackend::simd_cpu
        || backend == ExecutedBackend::vulkan_gpu;
}

/// True only for a concrete recomputed or persisted index source.
[[nodiscard]] constexpr bool is_concrete_target_index_source(
    TargetIndexSource source) noexcept {
    return source == TargetIndexSource::recomputed
        || source == TargetIndexSource::persisted;
}

/// Merge backend identities; any concrete disagreement becomes mixed-invalid.
[[nodiscard]] constexpr ExecutedBackend merge_backend_identity(
    ExecutedBackend lhs,
    ExecutedBackend rhs) noexcept {
    if (lhs == ExecutedBackend::none) return rhs;
    if (rhs == ExecutedBackend::none) return lhs;
    if (lhs == rhs) return lhs;
    return ExecutedBackend::mixed_invalid;
}

/// Merge index-source identities; any concrete disagreement becomes mixed-invalid.
[[nodiscard]] constexpr TargetIndexSource merge_target_index_identity(
    TargetIndexSource lhs,
    TargetIndexSource rhs) noexcept {
    if (lhs == TargetIndexSource::none) return rhs;
    if (rhs == TargetIndexSource::none) return lhs;
    if (lhs == rhs) return lhs;
    return TargetIndexSource::mixed_invalid;
}

/// True only for one concrete, operation-attestable SIMD chaining policy.
[[nodiscard]] constexpr bool is_concrete_simd_chaining_policy_identity(
    SimdChainingPolicyIdentity policy) noexcept {
    return policy == SimdChainingPolicyIdentity::exact_visited
        || policy
            == SimdChainingPolicyIdentity::bounded_batch_approximate;
}

/// Merge policy identities; a concrete disagreement remains visible and
/// revokes aggregate attestation instead of silently choosing either policy.
[[nodiscard]] constexpr SimdChainingPolicyIdentity
merge_simd_chaining_policy_identity(
    SimdChainingPolicyIdentity lhs,
    SimdChainingPolicyIdentity rhs) noexcept {
    if (lhs == SimdChainingPolicyIdentity::none) return rhs;
    if (rhs == SimdChainingPolicyIdentity::none) return lhs;
    if (lhs == rhs) return lhs;
    return SimdChainingPolicyIdentity::mixed_invalid;
}

[[nodiscard]] constexpr bool is_concrete_simd_alignment_algorithm_identity(
    SimdAlignmentAlgorithmIdentity algorithm) noexcept {
    return algorithm == SimdAlignmentAlgorithmIdentity::block_aligner;
}

[[nodiscard]] constexpr SimdAlignmentAlgorithmIdentity
merge_simd_alignment_algorithm_identity(
    SimdAlignmentAlgorithmIdentity lhs,
    SimdAlignmentAlgorithmIdentity rhs) noexcept {
    if (lhs == SimdAlignmentAlgorithmIdentity::none) return rhs;
    if (rhs == SimdAlignmentAlgorithmIdentity::none) return lhs;
    if (lhs == rhs) return lhs;
    return SimdAlignmentAlgorithmIdentity::mixed_invalid;
}

/// Only an all-SIMD backend may publish SIMD base-alignment identity, and its
/// requested and operation-observed identities must both be Block Aligner.
[[nodiscard]] constexpr bool simd_alignment_algorithm_identity_valid_for_backend(
    ExecutedBackend backend,
    SimdAlignmentAlgorithmIdentity requested,
    SimdAlignmentAlgorithmIdentity observed) noexcept {
    if (backend == ExecutedBackend::simd_cpu) {
        return is_concrete_simd_alignment_algorithm_identity(requested)
            && requested == observed;
    }
    return requested == SimdAlignmentAlgorithmIdentity::none
        && observed == SimdAlignmentAlgorithmIdentity::none;
}

/// A concrete all-SIMD job must retain the same requested and observed
/// policy. Scalar/GPU jobs must carry no SIMD-policy identity at all.
[[nodiscard]] constexpr bool simd_chaining_policy_identity_valid_for_backend(
    ExecutedBackend backend,
    SimdChainingPolicyIdentity requested,
    SimdChainingPolicyIdentity observed) noexcept {
    if (backend == ExecutedBackend::simd_cpu) {
        return is_concrete_simd_chaining_policy_identity(requested)
            && requested == observed;
    }
    return requested == SimdChainingPolicyIdentity::none
        && observed == SimdChainingPolicyIdentity::none;
}

/// Validate nonempty UTF-8 followed by one NUL and canonical zero padding.
[[nodiscard]] constexpr bool canonical_nonempty_driver_identity(
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
[[nodiscard]] constexpr bool environment_is_valid_for_backend(
    const ExecutionEnvironmentProof& environment,
    ExecutedBackend backend) noexcept {
    if (environment.mixed_invalid
        || environment.effective_cpu_workers == 0
        || environment.effective_cpu_workers > sql_integer_value_max
        || environment.gpu_pipeline_cache_seed_size > sql_bigint_value_max
        || !ExecutionEnvironmentProof::highway_fields_are_valid(environment)) {
        return false;
    }
    if (backend == ExecutedBackend::scalar_cpu || backend == ExecutedBackend::simd_cpu) {
        return !environment.gpu_environment_present
            && all_zero(environment.gpu_device_uuid)
            && all_zero(environment.gpu_driver_identity)
            && environment.gpu_pipeline_cache_policy == PipelineCachePolicy::not_applicable
            && all_zero(environment.gpu_pipeline_cache_seed_sha256)
            && environment.gpu_pipeline_cache_seed_size == 0
            && environment.gpu_pipeline_cache_manifest_checksum == 0
            && environment.gpu_pipeline_cache_hit == OptionalProofBool::not_applicable;
    }
    if (backend != ExecutedBackend::vulkan_gpu) return false;
    const bool common_gpu_identity_valid =
        environment.gpu_environment_present
        && !all_zero(environment.gpu_device_uuid)
        && canonical_nonempty_driver_identity(environment.gpu_driver_identity)
        && !all_zero(environment.gpu_pipeline_cache_seed_sha256)
        && environment.gpu_pipeline_cache_manifest_checksum != 0;
    if (!common_gpu_identity_valid) return false;
    if (environment.gpu_pipeline_cache_policy
            == PipelineCachePolicy::immutable_preseed_all_hit) {
        return environment.gpu_pipeline_cache_seed_size > 0
            && environment.gpu_pipeline_cache_hit
                == OptionalProofBool::true_value;
    }
    if (environment.gpu_pipeline_cache_policy
            == PipelineCachePolicy::isolated_miss_diagnostic) {
        return environment.gpu_pipeline_cache_hit
            == OptionalProofBool::false_value;
    }
    return false;
}

/// Return the sole permitted engine for a stage/mode. ExecutionEngine::count
/// means that the stage must be entirely ineligible in this mode.
[[nodiscard]] constexpr ExecutionEngine expected_execution_engine(
    ExecutedBackend backend,
    TargetIndexSource source,
    PipelineStage stage) noexcept {
    if (!is_concrete_backend(backend) || !is_concrete_target_index_source(source)) {
        return ExecutionEngine::count;
    }
    const ExecutionEngine backend_engine = backend == ExecutedBackend::vulkan_gpu
        ? ExecutionEngine::vulkan_gpu
        : backend == ExecutedBackend::simd_cpu
            ? ExecutionEngine::simd_cpu
            : ExecutionEngine::scalar_cpu;
    switch (stage) {
        case PipelineStage::query_sketch:
            return backend_engine;
        case PipelineStage::target_sketch:
            return source == TargetIndexSource::recomputed
                ? backend_engine
                : ExecutionEngine::count;
        case PipelineStage::target_index_build:
            if (source != TargetIndexSource::recomputed) return ExecutionEngine::count;
            return backend == ExecutedBackend::vulkan_gpu
                ? ExecutionEngine::vulkan_gpu
                : ExecutionEngine::scalar_cpu;
        case PipelineStage::persisted_index_decode:
            return source == TargetIndexSource::persisted
                ? ExecutionEngine::host_io
                : ExecutionEngine::count;
        case PipelineStage::partition_selection:
            if (source != TargetIndexSource::persisted) return ExecutionEngine::count;
            return backend == ExecutedBackend::vulkan_gpu
                ? ExecutionEngine::vulkan_gpu
                : ExecutionEngine::scalar_cpu;
        case PipelineStage::device_index_upload:
            return source == TargetIndexSource::persisted
                    && backend == ExecutedBackend::vulkan_gpu
                ? ExecutionEngine::vulkan_gpu
                : ExecutionEngine::count;
        case PipelineStage::seed_lookup:
            return backend == ExecutedBackend::vulkan_gpu
                ? ExecutionEngine::vulkan_gpu
                : ExecutionEngine::scalar_cpu;
        case PipelineStage::chain_score:
        case PipelineStage::chain_backtrack:
            return backend_engine;
        case PipelineStage::region_generation:
        case PipelineStage::primary_secondary_reduction:
        case PipelineStage::divergence_estimation:
        case PipelineStage::mapq:
        case PipelineStage::target_window_plan:
        case PipelineStage::cigar_job_generation:
            return backend == ExecutedBackend::vulkan_gpu
                ? ExecutionEngine::vulkan_gpu
                : ExecutionEngine::scalar_cpu;
        case PipelineStage::target_window_io:
            return ExecutionEngine::host_io;
        case PipelineStage::target_window_upload:
            return backend == ExecutedBackend::vulkan_gpu
                ? ExecutionEngine::vulkan_gpu
                : ExecutionEngine::count;
        case PipelineStage::left_extension:
        case PipelineStage::gap_extension:
        case PipelineStage::right_extension:
            return backend_engine;
        case PipelineStage::count:
            return ExecutionEngine::count;
    }
    return ExecutionEngine::count;
}

/// Validate binary per-job counts and exclusive execution-engine attribution.
[[nodiscard]] constexpr bool stage_coverage_is_exclusive(
    const StageCoverage& coverage,
    ExecutionEngine expected_engine) noexcept {
    if (coverage.eligible_jobs > 1 || coverage.fallback_jobs > 1) return false;
    for (const std::uint64_t executed : coverage.executed_jobs) {
        if (executed > 1) return false;
    }
    if (coverage.fallback_jobs != 0) return false;
    if (expected_engine == ExecutionEngine::count) {
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
[[nodiscard]] constexpr bool stage_requires_front_end_eligibility(
    TargetIndexSource source,
    PipelineStage stage) noexcept {
    if (stage == PipelineStage::query_sketch) return true;
    if (source == TargetIndexSource::recomputed) {
        return stage == PipelineStage::target_sketch
            || stage == PipelineStage::target_index_build;
    }
    if (source == TargetIndexSource::persisted) {
        return stage == PipelineStage::persisted_index_decode
            || stage == PipelineStage::partition_selection;
    }
    return false;
}

/// Fail-closed validator for one complete logical target-query proof.
[[nodiscard]] constexpr bool validate_job_execution_stats(
    const PipelineJobExecutionStats& job) noexcept {
    if (!is_concrete_backend(job.requested_backend)
        || job.requested_backend != job.actual_backend
        || !simd_chaining_policy_identity_valid_for_backend(
            job.actual_backend,
            job.requested_simd_chaining_policy,
            job.observed_simd_chaining_policy)
        || !simd_alignment_algorithm_identity_valid_for_backend(
            job.actual_backend,
            job.requested_simd_alignment_algorithm,
            job.observed_simd_alignment_algorithm)
        || !is_concrete_target_index_source(job.requested_target_index_source)
        || job.requested_target_index_source != job.actual_target_index_source
        || ExecutionEnvironmentProof::highway_fields_are_absent(
            job.environment)
        || !environment_is_valid_for_backend(job.environment, job.actual_backend)) {
        return false;
    }
    if (job.logical_target_row > sql_bigint_value_max
        || job.query_ordinal > sql_bigint_value_max) {
        return false;
    }
    if (job.actual_target_index_source == TargetIndexSource::persisted) {
        if (job.persisted_generation_id == 0
            || job.persisted_generation_id > sql_bigint_value_max
            || job.persisted_source_identity == 0) {
            return false;
        }
    } else if (job.persisted_generation_id != 0 || job.persisted_source_identity != 0) {
        return false;
    }
    for (std::size_t i = 0; i < pipeline_stage_count; ++i) {
        const auto stage = static_cast<PipelineStage>(i);
        const ExecutionEngine expected_engine = expected_execution_engine(
            job.actual_backend, job.actual_target_index_source, stage);
        if (!stage_coverage_is_exclusive(
                job.stages.coverage[i],
                expected_engine)) {
            return false;
        }
        if (expected_engine != ExecutionEngine::count
            && stage_requires_front_end_eligibility(
                job.actual_target_index_source, stage)
            && job.stages.coverage[i].eligible_jobs != 1) {
            return false;
        }
    }
    return true;
}

/// Saturating statement/node aggregate of job and operational proof.
struct PipelineExecutionStats {
    ExecutedBackend requested_backend = ExecutedBackend::none;
    ExecutedBackend actual_backend = ExecutedBackend::none;
    SimdChainingPolicyIdentity requested_simd_chaining_policy =
        SimdChainingPolicyIdentity::none;
    SimdChainingPolicyIdentity observed_simd_chaining_policy =
        SimdChainingPolicyIdentity::none;
    SimdAlignmentAlgorithmIdentity requested_simd_alignment_algorithm =
        SimdAlignmentAlgorithmIdentity::none;
    SimdAlignmentAlgorithmIdentity observed_simd_alignment_algorithm =
        SimdAlignmentAlgorithmIdentity::none;
    TargetIndexSource requested_target_index_source = TargetIndexSource::none;
    TargetIndexSource actual_target_index_source = TargetIndexSource::none;
    std::uint64_t persisted_generation_id = 0;
    std::uint64_t persisted_source_identity = 0;
    bool mixed_persisted_identity = false;
    std::uint64_t attempted_alignment_jobs = 0;
    std::uint64_t fully_attested_alignment_jobs = 0;
    std::uint64_t unattested_alignment_jobs = 0;
    // Internal merge provenance. These are intentionally not part of the SQL
    // ABI: they let an aggregate-level identity/environment failure name the
    // first formerly-attested job that must now be reclassified.
    std::uint64_t first_attempted_target_row = unset_job_ordinal;
    std::uint64_t first_attempted_query_ordinal = unset_job_ordinal;
    std::uint64_t first_unattested_target_row = unset_job_ordinal;
    std::uint64_t first_unattested_query_ordinal = unset_job_ordinal;
    ExecutionEnvironmentProof environment{};
    PipelineJobStageCoverage stages{};
    PipelineOperationalStats operational{};

    /// Once aggregate arithmetic or a report-once identity becomes lossy, no
    /// subset may continue to appear fully attested. Preserve the first
    /// attempted coordinates so the fail-closed transition remains diagnosable.
    void reclassify_all_attempted_jobs() noexcept {
        if (attempted_alignment_jobs == 0) return;
        fully_attested_alignment_jobs = 0;
        unattested_alignment_jobs = attempted_alignment_jobs;
        // A global downgrade includes every earlier job, so its first failure
        // is necessarily the first attempt, not the later job that exposed the
        // drift or malformed proof.
        first_unattested_target_row = first_attempted_target_row;
        first_unattested_query_ordinal = first_attempted_query_ordinal;
    }

    /// Validate and merge one logical target-query proof exactly once.
    void merge_job(
        const PipelineJobExecutionStats& job) noexcept {
        bool attested = validate_job_execution_stats(job);
        if ((requested_backend != ExecutedBackend::none
                && requested_backend != job.requested_backend)
            || (actual_backend != ExecutedBackend::none
                && actual_backend != job.actual_backend)
            || (requested_simd_chaining_policy
                    != SimdChainingPolicyIdentity::none
                && requested_simd_chaining_policy
                    != job.requested_simd_chaining_policy)
            || (observed_simd_chaining_policy
                    != SimdChainingPolicyIdentity::none
                && observed_simd_chaining_policy
                    != job.observed_simd_chaining_policy)
            || (requested_simd_alignment_algorithm
                    != SimdAlignmentAlgorithmIdentity::none
                && requested_simd_alignment_algorithm
                    != job.requested_simd_alignment_algorithm)
            || (observed_simd_alignment_algorithm
                    != SimdAlignmentAlgorithmIdentity::none
                && observed_simd_alignment_algorithm
                    != job.observed_simd_alignment_algorithm)
            || (requested_target_index_source != TargetIndexSource::none
                && requested_target_index_source != job.requested_target_index_source)
            || (actual_target_index_source != TargetIndexSource::none
                && actual_target_index_source != job.actual_target_index_source)
            || (!ExecutionEnvironmentProof::is_unset(environment)
                && (!ExecutionEnvironmentProof::payload_compatible(
                        environment, job.environment)
                    || environment.mixed_invalid
                    || job.environment.mixed_invalid))) {
            attested = false;
        }

        if (job.actual_target_index_source == TargetIndexSource::persisted) {
            const bool complete_job_identity =
                job.persisted_generation_id != 0 && job.persisted_source_identity != 0;
            if (!complete_job_identity) {
                mixed_persisted_identity = true;
                attested = false;
            } else if (persisted_generation_id == 0 && persisted_source_identity == 0) {
                persisted_generation_id = job.persisted_generation_id;
                persisted_source_identity = job.persisted_source_identity;
            } else if (persisted_generation_id != job.persisted_generation_id
                       || persisted_source_identity != job.persisted_source_identity) {
                mixed_persisted_identity = true;
                attested = false;
            }
        } else if (job.persisted_generation_id != 0 || job.persisted_source_identity != 0) {
            mixed_persisted_identity = true;
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

        if (first_attempted_target_row == unset_job_ordinal) {
            first_attempted_target_row = job.logical_target_row;
            first_attempted_query_ordinal = job.query_ordinal;
        }
        bool saturated = merge_counter(attempted_alignment_jobs, 1);
        if (attested) {
            saturated = merge_counter(fully_attested_alignment_jobs, 1) || saturated;
        } else {
            saturated = merge_counter(unattested_alignment_jobs, 1) || saturated;
            if (first_unattested_target_row == unset_job_ordinal) {
                first_unattested_target_row = job.logical_target_row;
                first_unattested_query_ordinal = job.query_ordinal;
            }
        }
        saturated = stages.merge(job.stages) || saturated;
        operational.stats_saturated = operational.stats_saturated || saturated;
        if (!attested
            || unattested_alignment_jobs != 0
            || operational.stats_saturated
            || operational.operational_identity_conflict
            || operational.backend_declines != 0
            || operational.backend_fallbacks != 0
            || operational.simd_chaining_policy_rejections != 0
            || operational.simd_chaining_policy_evidence_saturated) {
            reclassify_all_attempted_jobs();
        }
    }

    /// Merge a once-per-buffer operational delta.
    void merge_operational(
        const PipelineOperationalStats& batch) noexcept {
        operational.merge(batch);
        if (operational.stats_saturated
            || operational.operational_identity_conflict
            || operational.backend_declines != 0
            || operational.backend_fallbacks != 0
            || operational.simd_chaining_policy_rejections != 0
            || operational.simd_chaining_policy_evidence_saturated) {
            reclassify_all_attempted_jobs();
        }
    }

    /// Detect actual Vulkan work before interpreting a mixed diagnostic's
    /// environment. Selection labels alone are intentionally insufficient.
    [[nodiscard]] static constexpr bool has_gpu_execution_evidence(
        const PipelineExecutionStats& stats) noexcept {
        const auto& operational = stats.operational;
        for (std::size_t i = 0; i < pipeline_stage_count; ++i) {
            if (stats.stages.coverage[i].executed_jobs[
                    engine_index(ExecutionEngine::vulkan_gpu)] != 0
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

    /// Validate an all-unattested diagnostic without promoting it to proof.
    [[nodiscard]] static constexpr bool diagnostic_environment_is_valid(
        const PipelineExecutionStats& stats) noexcept {
        if (stats.actual_backend != ExecutedBackend::mixed_invalid
            || stats.attempted_alignment_jobs == 0
            || stats.fully_attested_alignment_jobs != 0
            || stats.unattested_alignment_jobs
                != stats.attempted_alignment_jobs) {
            return false;
        }
        if (is_concrete_backend(stats.requested_backend)) {
            return environment_is_valid_for_backend(
                stats.environment, stats.requested_backend);
        }
        if (stats.requested_backend != ExecutedBackend::mixed_invalid) {
            return false;
        }
        const ExecutedBackend environment_backend =
            has_gpu_execution_evidence(stats)
            ? ExecutedBackend::vulkan_gpu
            : ExecutedBackend::scalar_cpu;
        return environment_is_valid_for_backend(
            stats.environment, environment_backend);
    }

    /// Validate environment presence before aggregate proof adoption.
    [[nodiscard]] static constexpr bool aggregate_environment_operand_is_valid(
        const PipelineExecutionStats& stats) noexcept {
        if (stats.attempted_alignment_jobs == 0) {
            return ExecutionEnvironmentProof::is_unset(stats.environment);
        }
        if (is_concrete_backend(stats.actual_backend)) {
            return environment_is_valid_for_backend(
                stats.environment, stats.actual_backend);
        }
        // During incremental attestation wiring, or for an intentionally
        // mixed algorithm tuple, execution may publish actual=mixed_invalid.
        // Preserve a truthful environment only when every job is explicitly
        // unattested. This can never satisfy backend acceptance, but it keeps
        // partial-statement diagnostics readable without laundering proof.
        return diagnostic_environment_is_valid(stats);
    }

    /// Validate the exact per-stage aggregate implied by a concrete
    /// backend/source and the number of attempted logical jobs. Optional,
    /// data-dependent stages may cover any subset; unconditional front-end
    /// stages must cover every job. No forbidden engine or fallback lane may
    /// carry work.
    [[nodiscard]] static constexpr bool aggregate_stage_coverage_is_valid(
        const PipelineExecutionStats& stats) noexcept {
        if (!is_concrete_backend(stats.actual_backend)
            || !is_concrete_target_index_source(
                stats.actual_target_index_source)) {
            return false;
        }
        for (std::size_t i = 0; i < pipeline_stage_count; ++i) {
            const auto stage = static_cast<PipelineStage>(i);
            const StageCoverage& coverage = stats.stages.coverage[i];
            const ExecutionEngine expected_engine = expected_execution_engine(
                stats.actual_backend, stats.actual_target_index_source, stage);
            if (coverage.fallback_jobs != 0
                || coverage.eligible_jobs > stats.attempted_alignment_jobs) {
                return false;
            }
            if (expected_engine == ExecutionEngine::count) {
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
            if (stage_requires_front_end_eligibility(
                    stats.actual_target_index_source, stage)
                && coverage.eligible_jobs
                    != stats.attempted_alignment_jobs) {
                return false;
            }
        }
        return true;
    }

    /// Canonical identity element for proof aggregation. A zero-job value with
    /// selected backend/source labels or a captured environment is not empty;
    /// allowing it to behave as the identity would erase malformed evidence.
    [[nodiscard]] static constexpr bool is_exact_empty_proof_operand(
        const PipelineExecutionStats& stats) noexcept {
        return stats.attempted_alignment_jobs == 0
            && stats.fully_attested_alignment_jobs == 0
            && stats.unattested_alignment_jobs == 0
            && stats.requested_backend == ExecutedBackend::none
            && stats.actual_backend == ExecutedBackend::none
            && stats.requested_simd_chaining_policy
                == SimdChainingPolicyIdentity::none
            && stats.observed_simd_chaining_policy
                == SimdChainingPolicyIdentity::none
            && stats.requested_simd_alignment_algorithm
                == SimdAlignmentAlgorithmIdentity::none
            && stats.observed_simd_alignment_algorithm
                == SimdAlignmentAlgorithmIdentity::none
            && stats.requested_target_index_source == TargetIndexSource::none
            && stats.actual_target_index_source == TargetIndexSource::none
            && stats.persisted_generation_id == 0
            && stats.persisted_source_identity == 0
            && !stats.mixed_persisted_identity
            && stats.first_attempted_target_row == unset_job_ordinal
            && stats.first_attempted_query_ordinal == unset_job_ordinal
            && stats.first_unattested_target_row == unset_job_ordinal
            && stats.first_unattested_query_ordinal == unset_job_ordinal
            && ExecutionEnvironmentProof::is_unset(stats.environment)
            && stats.stages == PipelineJobStageCoverage{}
            && stats.operational == PipelineOperationalStats{};
    }

    /// Only a canonical empty value or an internally complete concrete proof
    /// may retain fully-attested counts across an aggregate merge. Mixed
    /// diagnostics remain readable, but can never promote a clean sibling.
    [[nodiscard]] static constexpr bool can_retain_full_attestation(
        const PipelineExecutionStats& stats) noexcept {
        if (is_exact_empty_proof_operand(stats)) return true;
        if (stats.attempted_alignment_jobs == 0
            || stats.fully_attested_alignment_jobs
                != stats.attempted_alignment_jobs
            || stats.unattested_alignment_jobs != 0
            || stats.first_attempted_target_row == unset_job_ordinal
            || stats.first_attempted_query_ordinal == unset_job_ordinal
            || stats.first_unattested_target_row != unset_job_ordinal
            || stats.first_unattested_query_ordinal != unset_job_ordinal
            || stats.requested_backend != stats.actual_backend
            || !is_concrete_backend(stats.actual_backend)
            || !simd_chaining_policy_identity_valid_for_backend(
                stats.actual_backend,
                stats.requested_simd_chaining_policy,
                stats.observed_simd_chaining_policy)
            || !simd_alignment_algorithm_identity_valid_for_backend(
                stats.actual_backend,
                stats.requested_simd_alignment_algorithm,
                stats.observed_simd_alignment_algorithm)
            || stats.requested_target_index_source
                != stats.actual_target_index_source
            || !is_concrete_target_index_source(
                stats.actual_target_index_source)
            || stats.mixed_persisted_identity
            || stats.operational.operational_identity_conflict
            || stats.operational.stats_saturated
            || stats.operational.backend_declines != 0
            || stats.operational.backend_fallbacks != 0
            || stats.operational.simd_chaining_policy_rejections != 0
            || stats.operational.simd_chaining_policy_evidence_saturated
            || ExecutionEnvironmentProof::highway_fields_are_absent(
                stats.environment)
            || !environment_is_valid_for_backend(
                stats.environment, stats.actual_backend)
            || !aggregate_stage_coverage_is_valid(stats)) {
            return false;
        }
        if (stats.actual_target_index_source == TargetIndexSource::persisted) {
            return stats.persisted_generation_id != 0
                && stats.persisted_generation_id <= sql_bigint_value_max
                && stats.persisted_source_identity != 0;
        }
        return stats.persisted_generation_id == 0
            && stats.persisted_source_identity == 0;
    }

    /// Merge one completed buffer/node aggregate without laundering proof gaps.
    void merge(
        const PipelineExecutionStats& rhs) noexcept {
        // Validate both operands before first-proof adoption. In particular, a
        // missing environment on a nonempty left accumulator must not disappear
        // merely because the right operand has a valid proof.
        const bool lhs_environment_valid = aggregate_environment_operand_is_valid(*this);
        const bool rhs_environment_valid = aggregate_environment_operand_is_valid(rhs);
        const bool environment_drift = lhs_environment_valid
            && rhs_environment_valid
            && attempted_alignment_jobs != 0
            && rhs.attempted_alignment_jobs != 0
            && !ExecutionEnvironmentProof::payload_compatible(
                environment, rhs.environment);
        const bool lhs_empty = is_exact_empty_proof_operand(*this);
        const bool rhs_empty = is_exact_empty_proof_operand(rhs);
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
                && mixed_persisted_identity == rhs.mixed_persisted_identity);
        // The exact empty operand is a two-sided identity, but two completed
        // proofs that name different nonzero Highway bindings are not
        // compatible proof shards. ExecutionEnvironmentProof::merge()
        // truthfully records that case as mixed; revoke full attestation in
        // this same two-operand merge instead of waiting for a later operand.
        const bool highway_binding_identity_compatible = lhs_empty || rhs_empty
            || (environment.highway_executed_target
                    == rhs.environment.highway_executed_target
                && environment.highway_executed_target_mixed
                    == rhs.environment.highway_executed_target_mixed);
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
                    first_unattested_target_row != unset_job_ordinal
                    ? first_unattested_target_row
                    : first_attempted_target_row;
                first_unattested_query_ordinal =
                    first_unattested_query_ordinal != unset_job_ordinal
                    ? first_unattested_query_ordinal
                    : first_attempted_query_ordinal;
            } else {
                first_unattested_target_row =
                    rhs.first_unattested_target_row != unset_job_ordinal
                    ? rhs.first_unattested_target_row
                    : rhs.first_attempted_target_row;
                first_unattested_query_ordinal =
                    rhs.first_unattested_query_ordinal != unset_job_ordinal
                    ? rhs.first_unattested_query_ordinal
                    : rhs.first_attempted_query_ordinal;
            }
        }

        const auto identity_invalid = [](
            TargetIndexSource source,
            std::uint64_t generation,
            std::uint64_t identity) noexcept {
            if (source == TargetIndexSource::persisted) {
                return generation == 0 || identity == 0;
            }
            if (source == TargetIndexSource::recomputed
                || source == TargetIndexSource::none) {
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
            mixed_persisted_identity = true;
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
            mixed_persisted_identity = true;
        }
        if (rhs.persisted_generation_id != 0 && rhs.persisted_source_identity != 0) {
            if (persisted_generation_id == 0 && persisted_source_identity == 0) {
                persisted_generation_id = rhs.persisted_generation_id;
                persisted_source_identity = rhs.persisted_source_identity;
            } else if (persisted_generation_id != rhs.persisted_generation_id
                       || persisted_source_identity != rhs.persisted_source_identity) {
                mixed_persisted_identity = true;
            }
        }
        mixed_persisted_identity = mixed_persisted_identity || rhs.mixed_persisted_identity;

        if (!ExecutionEnvironmentProof::is_unset(rhs.environment)
            || rhs.attempted_alignment_jobs != 0) {
            environment.merge(rhs.environment);
        }
        if (!lhs_environment_valid || !rhs_environment_valid || environment_drift) {
            environment.mixed_invalid = true;
        }

        bool saturated = merge_counter(
            attempted_alignment_jobs, rhs.attempted_alignment_jobs);
        saturated = merge_counter(
            fully_attested_alignment_jobs, rhs_fully_attested) || saturated;
        saturated = merge_counter(
            unattested_alignment_jobs, rhs_unattested) || saturated;
        saturated = stages.merge(rhs.stages) || saturated;
        if (first_unattested_target_row == unset_job_ordinal
            && rhs.first_unattested_target_row != unset_job_ordinal) {
            first_unattested_target_row = rhs.first_unattested_target_row;
            first_unattested_query_ordinal = rhs.first_unattested_query_ordinal;
        }
        if (first_attempted_target_row == unset_job_ordinal
            && rhs.first_attempted_target_row != unset_job_ordinal) {
            first_attempted_target_row = rhs.first_attempted_target_row;
            first_attempted_query_ordinal = rhs.first_attempted_query_ordinal;
        }
        operational.merge(rhs.operational);
        operational.stats_saturated = operational.stats_saturated || saturated;
        if (operational.stats_saturated
            || operational.operational_identity_conflict
            || operational.backend_declines != 0
            || operational.backend_fallbacks != 0
            || operational.simd_chaining_policy_rejections != 0
            || operational.simd_chaining_policy_evidence_saturated
            || requested_simd_chaining_policy
                == SimdChainingPolicyIdentity::mixed_invalid
            || observed_simd_chaining_policy
                == SimdChainingPolicyIdentity::mixed_invalid
            || requested_simd_alignment_algorithm
                == SimdAlignmentAlgorithmIdentity::mixed_invalid
            || observed_simd_alignment_algorithm
                == SimdAlignmentAlgorithmIdentity::mixed_invalid) {
            reclassify_all_attempted_jobs();
        }
    }
};

/// Fixed publication equation checked by the PostgreSQL owner before one
/// persisted Shape-V5 GPU wave becomes visible.  `consumed_jobs` alone is not
/// authority: a mismatched expectation deliberately burns its record, so a
/// proof may be fully consumed while still being ineligible for publication.
struct PersistedShapeV5CoordinatePublicationCounts {
    std::uint64_t expected_jobs = 0;
    std::uint64_t proof_jobs = 0;
    std::uint64_t consumed_jobs = 0;
    std::uint64_t accepted_jobs = 0;
    std::uint64_t burned_jobs = 0;
    std::uint64_t pending_jobs = 0;
    std::uint64_t staged_jobs = 0;
};

[[nodiscard]] constexpr bool
persisted_shape_v5_coordinate_publication_counts_valid(
    const PersistedShapeV5CoordinatePublicationCounts& counts) noexcept {
    return counts.expected_jobs != 0
        && counts.expected_jobs <= sql_bigint_value_max
        && counts.proof_jobs == counts.expected_jobs
        && counts.consumed_jobs == counts.expected_jobs
        && counts.accepted_jobs == counts.expected_jobs
        && counts.burned_jobs == 0
        && counts.pending_jobs == 0
        && counts.staged_jobs == counts.expected_jobs;
}

static_assert(std::is_trivially_copyable_v<PipelineExecutionStats>);
static_assert(std::is_standard_layout_v<PipelineExecutionStats>);

/// Maximum statement-local persisted-GPU evidence retained for SQL proof.
/// This is the production dispatch ceiling, not a silently growing cache.
inline constexpr std::uint64_t
    persisted_gpu_operation_evidence_capacity = 8192u;

/// One causal queue submission, represented without Vulkan handles/pointers.
struct PersistedGpuStructuredSubmissionEvidence {
    std::uint64_t operation_identity = 0u;
    std::uint64_t ordinal = 0u;
    std::uint64_t submit_wait_identity = 0u;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return operation_identity != 0u && ordinal != 0u
            && ordinal <= sql_bigint_value_max
            && submit_wait_identity != 0u;
    }
    [[nodiscard]] constexpr bool operator==(
        const PersistedGpuStructuredSubmissionEvidence&) const noexcept =
        default;
};

/// Immutable numeric Vulkan/cache identity copied from the runtime proof.
struct PersistedGpuEnvironmentEvidence {
    std::array<std::uint8_t, 16> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, 16> pipeline_cache_uuid{};
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;

    [[nodiscard]] bool accepted() const noexcept {
        return api_version != 0u && driver_version != 0u
            && seed_size != 0u && seed_size <= sql_bigint_value_max
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
        const PersistedGpuEnvironmentEvidence&) const noexcept = default;
};

inline constexpr std::size_t
    persisted_gpu_helper_build_identity_capacity = 128u;
inline constexpr std::size_t persisted_gpu_helper_source_capacity = 32u;

/// Process-wide immutable Metal archive qualification copied without paths,
/// file descriptors, API handles or pointers. Text fields are bounded ASCII
/// images so the admitted operation chunk remains a fixed allocation.
struct PersistedGpuCacheQualificationEvidence {
    std::uint32_t version = 0u;
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
    std::uint32_t helper_protocol_version = 0u;
    std::array<char, persisted_gpu_helper_build_identity_capacity>
        helper_build_identity{};
    std::uint32_t helper_build_identity_size = 0u;
    std::array<char, persisted_gpu_helper_source_capacity> helper_source{};
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
    bool saturated = false;

    [[nodiscard]] std::string_view helper_build_identity_text()
        const noexcept {
        return {helper_build_identity.data(), helper_build_identity_size};
    }
    [[nodiscard]] std::string_view helper_source_text() const noexcept {
        return {helper_source.data(), helper_source_size};
    }

    [[nodiscard]] bool accepted() const noexcept {
        const auto nonzero = [](const auto& bytes) noexcept {
            return std::any_of(
                bytes.begin(), bytes.end(),
                [](const auto value) { return value != 0u; });
        };
        const auto safe_text = [](const auto& bytes,
                                  const std::uint32_t size) noexcept {
            if (size == 0u || size > bytes.size()) return false;
            for (std::size_t i = 0u; i < size; ++i) {
                const auto value = static_cast<unsigned char>(bytes[i]);
                if (value < 0x20u || value > 0x7eu
                    || value == static_cast<unsigned char>('"')
                    || value == static_cast<unsigned char>('\\')) {
                    return false;
                }
            }
            return std::all_of(
                bytes.begin() + size, bytes.end(),
                [](const char value) { return value == '\0'; });
        };
        return version == 2u && identity != 0u
            && archive_count != 0u
            && archive_count <= sql_bigint_value_max
            && archive_bytes != 0u
            && archive_bytes <= sql_bigint_value_max
            && archive_checksum != 0u
            && receipt_count == archive_count
            && hash_passes == 1u && hashed_bytes == archive_bytes
            && private_files_created == archive_count
            && private_files_unlinked == archive_count
            && live_private_paths == 0u
            && pipeline_filesystem_hash_passes == 0u
            && pipeline_filesystem_hashed_bytes == 0u
            && helper_protocol_version == 2u
            && safe_text(
                helper_build_identity, helper_build_identity_size)
            && helper_build_identity_text()
                == "bioseq_gpu_archive_builder_v2"
            && safe_text(helper_source, helper_source_size)
            && helper_source_text() == "installed-sibling"
            && helper_executable_size != 0u
            && helper_executable_size <= sql_bigint_value_max
            && nonzero(helper_executable_sha256)
            && helper_metal_registry_id != 0u
            && nonzero(device_uuid) && api_version != 0u
            && driver_version != 0u
            && nonzero(pipeline_cache_uuid)
            && seed_receipt_identity != 0u && nonzero(seed_sha256)
            && seed_size != 0u && seed_size <= sql_bigint_value_max
            && manifest_checksum != 0u && !saturated;
    }
    [[nodiscard]] constexpr bool operator==(
        const PersistedGpuCacheQualificationEvidence&) const noexcept =
        default;
};

/// Report-once cache counters attached to the accepted operation authority.
struct PersistedGpuCacheMetricsEvidence {
    std::uint64_t token_identity = 0u;
    std::uint64_t pipeline_cache_loads = 0u;
    std::uint64_t pipeline_cache_seed_bytes = 0u;
    std::uint64_t pipeline_cache_load_ns = 0u;
    std::uint64_t pipeline_creates = 0u;
    std::uint64_t pipeline_creation_ns = 0u;

    [[nodiscard]] constexpr bool accepted() const noexcept {
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
            && pipeline_cache_loads <= sql_bigint_value_max
            && pipeline_cache_seed_bytes <= sql_bigint_value_max
            && pipeline_cache_load_ns <= sql_bigint_value_max
            && pipeline_creates <= sql_bigint_value_max
            && pipeline_creation_ns <= sql_bigint_value_max;
    }
    [[nodiscard]] constexpr bool operator==(
        const PersistedGpuCacheMetricsEvidence&) const noexcept = default;
};

/// Coordinate result copied after the runtime's status/readback checks.
struct PersistedGpuCoordinateResultEvidence {
    std::uint64_t query_start = 0u;
    std::uint64_t query_end = 0u;
    std::uint64_t target_start = 0u;
    std::uint64_t target_end = 0u;
    std::uint64_t match_length = 0u;
    std::uint64_t block_length = 0u;
    std::int32_t chain_score = 0;
    std::int32_t seed_count = 0;
    bool is_reverse = false;
    bool valid = false;

    [[nodiscard]] constexpr bool accepted(
        const std::uint64_t query_length,
        const std::uint64_t target_length) const noexcept {
        return valid && chain_score > 0 && seed_count > 0
            && query_start < query_end && query_end <= query_length
            && target_start < target_end && target_end <= target_length
            && match_length != 0u
            && match_length <= block_length
            && match_length <= query_end - query_start
            && match_length <= target_end - target_start
            && block_length <= query_end - query_start
                + target_end - target_start
            && query_start <= sql_bigint_value_max
            && query_end <= sql_bigint_value_max
            && target_start <= sql_bigint_value_max
            && target_end <= sql_bigint_value_max
            && match_length <= sql_bigint_value_max
            && block_length <= sql_bigint_value_max;
    }
};

/// Full-stage causal evidence for one logical target/query coordinate result.
struct PersistedGpuJobEvidence {
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
    std::array<PersistedGpuStructuredSubmissionEvidence, 4> submissions{};
    std::uint64_t count_status_checks = 0u;
    std::uint64_t emit_status_checks = 0u;
    std::uint32_t status_or = 0u;
    std::uint64_t result_readbacks = 0u;
    std::uint64_t coordinate_vote_dispatches = 0u;
    std::uint64_t cpu_or_simd_biological_calls = 0u;
    std::uint64_t recompute_calls = 0u;
    std::uint64_t fallback_calls = 0u;
    PersistedGpuCoordinateResultEvidence coordinate{};

    [[nodiscard]] constexpr bool accepted(
        const std::uint64_t shape_operation_identity) const noexcept {
        if (logical_target_row > sql_bigint_value_max
            || logical_query_ordinal > sql_bigint_value_max
            || query_length == 0u || query_length > sql_bigint_value_max
            || target_length == 0u || target_length > sql_bigint_value_max
            || query_minimizer_count == 0u
            || query_minimizer_count > sql_bigint_value_max
            || query_tile_count == 0u
            || query_tile_count > sql_bigint_value_max
            || query_count_dispatches != 1u
            || query_scan_dispatches != 3u
            || query_emit_dispatches != 1u
            || query_completed_roundtrips != 2u
            || direct_count_dispatches != 1u
            || direct_scan_dispatches != 3u
            || anchor_count == 0u || anchor_count > sql_bigint_value_max
            || direct_emit_dispatches != 1u
            || sort_operations != 1u
            || range_build_dispatches != 1u
            || range_count == 0u || range_count > sql_bigint_value_max
            || chain_dispatches != 1u
            || chain_backtrack_dispatches != 1u
            || coordinate_dispatches != 1u
            || count_status_checks != 1u
            || emit_status_checks != 1u
            || status_or != 0u || result_readbacks != 1u
            || coordinate_vote_dispatches != 0u
            || cpu_or_simd_biological_calls != 0u
            || recompute_calls != 0u || fallback_calls != 0u
            || !coordinate.accepted(query_length, target_length)) {
            return false;
        }
        for (std::size_t i = 0u; i < submissions.size(); ++i) {
            if (!submissions[i].accepted()
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
struct PersistedGpuOperationEvidence {
    std::uint64_t operation_identity = 0u;
    PersistedShapeV5CoordinatePublicationCounts publication{};
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
    PersistedGpuEnvironmentEvidence environment{};
    PersistedGpuCacheQualificationEvidence cache_qualification{};
    PersistedGpuCacheMetricsEvidence cache_metrics{};

    [[nodiscard]] bool accepted(
        const std::span<const PersistedGpuJobEvidence> jobs) const noexcept {
        if (operation_identity == 0u
            || !persisted_shape_v5_coordinate_publication_counts_valid(
                publication)
            || published_jobs != publication.expected_jobs
            || publication_commits != 1u
            || query_pipeline_receipt_count != 3u
            || shape_pipeline_receipt_count < 7u
            || shape_pipeline_receipt_count > 10u
            || submission_count < 6u
            || submission_count > sql_bigint_value_max
            || query_pipeline_operation_identity == 0u
            || shape_pipeline_operation_identity == 0u
            || shape_pipeline_operation_identity != operation_identity
            || query_pipeline_operation_identity
                == shape_pipeline_operation_identity
            || query_submission_ordinals[0] == 0u
            || query_submission_ordinals[1] == 0u
            || query_submission_ordinals[0]
                == query_submission_ordinals[1]
            || query_submission_ordinals[0] > sql_bigint_value_max
            || query_submission_ordinals[1] > sql_bigint_value_max
            || seed_receipt_identity == 0u
            || generation_id == 0u
            || generation_id > sql_bigint_value_max
            || source_identity == 0u || cold_load_identity == 0u
            || !environment.accepted() || !cache_qualification.accepted()
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
            || !cache_metrics.accepted()
            || cache_metrics.token_identity != operation_identity
            || jobs.size() != publication.expected_jobs) {
            return false;
        }
        std::uint64_t shape_ordinal_count = 0u;
        for (std::size_t job_index = 0u; job_index < jobs.size(); ++job_index) {
            const PersistedGpuJobEvidence& job = jobs[job_index];
            if (!job.accepted(shape_pipeline_operation_identity)) return false;
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
                    if (shape_ordinal_count == sql_bigint_value_max) {
                        return false;
                    }
                    ++shape_ordinal_count;
                }
            }
        }
        return submission_count == 2u + shape_ordinal_count;
    }
};

class PersistedGpuOperationEvidenceLedger;

/// Pre-finalizer authority proving one sealed chunk can be linked without any
/// later allocation, duplicate search, range check, or capacity decision.
class PersistedGpuOperationEvidenceAppendAuthority {
public:
    PersistedGpuOperationEvidenceAppendAuthority() = default;
    PersistedGpuOperationEvidenceAppendAuthority(
        const PersistedGpuOperationEvidenceAppendAuthority&) = delete;
    PersistedGpuOperationEvidenceAppendAuthority& operator=(
        const PersistedGpuOperationEvidenceAppendAuthority&) = delete;
    PersistedGpuOperationEvidenceAppendAuthority(
        PersistedGpuOperationEvidenceAppendAuthority&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)) {}
    PersistedGpuOperationEvidenceAppendAuthority& operator=(
        PersistedGpuOperationEvidenceAppendAuthority&& other) noexcept {
        if (this != &other) {
            owner_ = std::exchange(other.owner_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] bool accepted() const noexcept { return owner_ != nullptr; }

private:
    explicit PersistedGpuOperationEvidenceAppendAuthority(
        PersistedGpuOperationEvidenceLedger& owner) noexcept
        : owner_(&owner) {}

    PersistedGpuOperationEvidenceLedger* owner_ = nullptr;
    friend class PersistedGpuOperationEvidenceLedger;
};

/// One admitted operation chunk. The job vector dies before its permit, and
/// the ledger removes next links iteratively before destroying each chunk.
class PersistedGpuOperationEvidenceChunk {
public:
    PersistedGpuOperationEvidenceChunk() = default;
    PersistedGpuOperationEvidenceChunk(
        const PersistedGpuOperationEvidenceChunk&) = delete;
    PersistedGpuOperationEvidenceChunk& operator=(
        const PersistedGpuOperationEvidenceChunk&) = delete;
    PersistedGpuOperationEvidenceChunk(
        PersistedGpuOperationEvidenceChunk&&) = delete;
    PersistedGpuOperationEvidenceChunk& operator=(
        PersistedGpuOperationEvidenceChunk&&) = delete;

    /// Admit and allocate one exact operation/job image before finalization.
    [[nodiscard]] static std::unique_ptr<
        PersistedGpuOperationEvidenceChunk> prepare(
            const std::size_t job_count) noexcept {
        using bioseq::parallelism::saturating_add;
        using bioseq::parallelism::saturating_mul;
        if (job_count == 0u
            || job_count > persisted_gpu_operation_evidence_capacity
            || job_count > std::vector<PersistedGpuJobEvidence>{}.max_size()) {
            return nullptr;
        }
        const std::uint64_t retained = saturating_add(
            sizeof(PersistedGpuOperationEvidenceChunk),
            saturating_mul(
                static_cast<std::uint64_t>(job_count),
                sizeof(PersistedGpuJobEvidence)));
        const std::uint64_t peak = saturating_mul(
            retained,
            bioseq::parallelism::kVectorPreallocationHeadroomFactor);
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || peak == (std::numeric_limits<std::uint64_t>::max)()) {
            return nullptr;
        }
        bioseq::parallelism::MemoryAdmissionFailure failure{};
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(peak, &failure);
        if (!permit) return nullptr;
        try {
            auto chunk = std::make_unique<
                PersistedGpuOperationEvidenceChunk>();
            chunk->capacity_permit_ = std::move(*permit);
            chunk->jobs_.resize(job_count);
            const std::uint64_t actual = saturating_add(
                sizeof(PersistedGpuOperationEvidenceChunk),
                saturating_mul(
                    static_cast<std::uint64_t>(chunk->jobs_.capacity()),
                    sizeof(PersistedGpuJobEvidence)));
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

    [[nodiscard]] PersistedGpuOperationEvidence& operation() noexcept {
        return operation_;
    }
    [[nodiscard]] std::span<PersistedGpuJobEvidence> jobs() noexcept {
        return jobs_;
    }
    [[nodiscard]] const PersistedGpuOperationEvidence& operation()
        const noexcept { return operation_; }
    [[nodiscard]] std::span<const PersistedGpuJobEvidence> jobs()
        const noexcept { return jobs_; }
    [[nodiscard]] const PersistedGpuOperationEvidenceChunk* next()
        const noexcept { return next_.get(); }

    /// Freeze populated fields after every full-stage/range/equation check.
    [[nodiscard]] bool seal() noexcept {
        sealed_ = operation_.accepted(jobs_);
        return sealed_;
    }
    [[nodiscard]] bool sealed() const noexcept {
        return sealed_;
    }

    /// Publish the Runtime-prevalidated final report-once delta. Runtime's
    /// claim plan proves componentwise boundedness before its irreversible
    /// report-once commit, so this post-finalizer step is only a fixed write.
    void publish_cache_metrics(
        const PersistedGpuCacheMetricsEvidence& metrics) noexcept {
        operation_.cache_metrics = metrics;
        cache_metrics_published_ = true;
    }
    [[nodiscard]] bool cache_metrics_published() const noexcept {
        return cache_metrics_published_;
    }

private:
    // Declared first: jobs/next are destroyed before their admission permit.
    bioseq::parallelism::MemoryPermit capacity_permit_{};
    std::unique_ptr<PersistedGpuOperationEvidenceChunk> next_{};
    std::vector<PersistedGpuJobEvidence> jobs_{};
    PersistedGpuOperationEvidence operation_{};
    bool cache_metrics_published_ = false;
    bool sealed_ = false;
    friend class PersistedGpuOperationEvidenceLedger;
};

/// Statement-root-owned causal evidence ledger. It never owns through stats;
/// schedulers retain only a borrowed pointer to this root object.
class PersistedGpuOperationEvidenceLedger {
public:
    PersistedGpuOperationEvidenceLedger() = default;
    PersistedGpuOperationEvidenceLedger(
        const PersistedGpuOperationEvidenceLedger&) = delete;
    PersistedGpuOperationEvidenceLedger& operator=(
        const PersistedGpuOperationEvidenceLedger&) = delete;
    PersistedGpuOperationEvidenceLedger(
        PersistedGpuOperationEvidenceLedger&& other) noexcept {
        move_from(other);
    }
    PersistedGpuOperationEvidenceLedger& operator=(
        PersistedGpuOperationEvidenceLedger&& other) noexcept {
        if (this != &other) {
            clear_iterative();
            move_from(other);
        }
        return *this;
    }
    ~PersistedGpuOperationEvidenceLedger() { clear_iterative(); }

    /// Complete all duplicate/capacity/range checks before runtime finalizer.
    [[nodiscard]] PersistedGpuOperationEvidenceAppendAuthority prepare_append(
        const std::unique_ptr<PersistedGpuOperationEvidenceChunk>& chunk)
        noexcept {
        if (frozen_ || invalid_ || chunk == nullptr || !chunk->sealed()
            || chunk->cache_metrics_published()
            || operation_count_ >= persisted_gpu_operation_evidence_capacity
            || job_count_ > persisted_gpu_operation_evidence_capacity
                - chunk->jobs().size()) {
            invalidate(chunk == nullptr ? 0u : 1u, true);
            return {};
        }
        const auto& operation = chunk->operation();
        for (const PersistedGpuOperationEvidenceChunk* existing = head_.get();
             existing != nullptr; existing = existing->next()) {
            const auto& old = existing->operation();
            if (old.operation_identity == operation.operation_identity
                || old.query_pipeline_operation_identity
                    == operation.query_pipeline_operation_identity
                || old.query_pipeline_operation_identity
                    == operation.shape_pipeline_operation_identity
                || old.shape_pipeline_operation_identity
                    == operation.query_pipeline_operation_identity) {
                invalidate(1u, false);
                return {};
            }
            if (old.generation_id != operation.generation_id
                || old.source_identity != operation.source_identity
                || old.cold_load_identity != operation.cold_load_identity
                || old.seed_receipt_identity
                    != operation.seed_receipt_identity
                || old.environment != operation.environment
                || old.cache_qualification
                    != operation.cache_qualification) {
                invalidate(1u, false);
                return {};
            }
        }
        return PersistedGpuOperationEvidenceAppendAuthority{*this};
    }

    /// Allocation-free/noexcept link after accepted Runtime authority exists.
    void commit_append(
        PersistedGpuOperationEvidenceAppendAuthority&& authority,
        std::unique_ptr<PersistedGpuOperationEvidenceChunk>&& chunk) noexcept {
        DEBUG_ASSERT(authority.owner_ == this,
            "persisted GPU evidence append authority owner changed");
        DEBUG_ASSERT(chunk != nullptr && chunk->sealed()
            && chunk->cache_metrics_published(),
            "persisted GPU evidence chunk was not publication-ready");
        PersistedGpuOperationEvidenceChunk* appended = chunk.get();
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
    void freeze() noexcept { frozen_ = true; }

    [[nodiscard]] bool complete() const noexcept {
        if (!frozen_ || invalid_ || saturated_ || dropped_operation_count_ != 0u
            || operation_count_ > persisted_gpu_operation_evidence_capacity
            || job_count_ > persisted_gpu_operation_evidence_capacity) {
            return false;
        }
        std::uint64_t operations = 0u;
        std::uint64_t jobs = 0u;
        for (const PersistedGpuOperationEvidenceChunk* chunk = head_.get();
             chunk != nullptr; chunk = chunk->next()) {
            if (!chunk->sealed() || !chunk->cache_metrics_published()) {
                return false;
            }
            ++operations;
            jobs += chunk->jobs().size();
        }
        return operations == operation_count_ && jobs == job_count_;
    }
    [[nodiscard]] bool accepted() const noexcept {
        return operation_count_ != 0u && job_count_ != 0u && complete();
    }
    /// Bind the immutable ledger back to the canonical statement aggregate.
    [[nodiscard]] bool matches_pipeline(
        const PipelineExecutionStats& stats) const noexcept {
        if (!accepted()
            || !PipelineExecutionStats::can_retain_full_attestation(stats)
            || stats.actual_backend != ExecutedBackend::vulkan_gpu
            || stats.actual_target_index_source != TargetIndexSource::persisted
            || stats.attempted_alignment_jobs != job_count_
            || stats.fully_attested_alignment_jobs != job_count_
            || stats.persisted_generation_id == 0u
            || stats.persisted_source_identity == 0u
            || !environment_is_valid_for_backend(
                stats.environment, ExecutedBackend::vulkan_gpu)) {
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
        for (const PersistedGpuOperationEvidenceChunk* chunk = head_.get();
             chunk != nullptr; chunk = chunk->next()) {
            const auto& operation = chunk->operation();
            ::bioseq::gpu::vulkan::VulkanFeatureSet identity{};
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
                || !operation.cache_qualification.accepted()
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
    [[nodiscard]] bool saturated() const noexcept { return saturated_; }
    [[nodiscard]] std::uint64_t dropped_operation_count() const noexcept {
        return dropped_operation_count_;
    }
    [[nodiscard]] std::uint64_t operation_count() const noexcept {
        return operation_count_;
    }
    [[nodiscard]] std::uint64_t job_count() const noexcept {
        return job_count_;
    }
    [[nodiscard]] const PersistedGpuOperationEvidenceChunk* head()
        const noexcept { return head_.get(); }

private:
    void invalidate(
        const std::uint64_t dropped,
        const bool saturated) noexcept {
        invalid_ = true;
        saturated_ = saturated_ || saturated;
        if (dropped > (std::numeric_limits<std::uint64_t>::max)()
                - dropped_operation_count_) {
            dropped_operation_count_ =
                (std::numeric_limits<std::uint64_t>::max)();
            saturated_ = true;
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

    void move_from(PersistedGpuOperationEvidenceLedger& other) noexcept {
        head_ = std::move(other.head_);
        tail_ = std::exchange(other.tail_, nullptr);
        operation_count_ = std::exchange(other.operation_count_, 0u);
        job_count_ = std::exchange(other.job_count_, 0u);
        dropped_operation_count_ = std::exchange(
            other.dropped_operation_count_, 0u);
        invalid_ = std::exchange(other.invalid_, false);
        saturated_ = std::exchange(other.saturated_, false);
        frozen_ = std::exchange(other.frozen_, false);
    }

    std::unique_ptr<PersistedGpuOperationEvidenceChunk> head_{};
    PersistedGpuOperationEvidenceChunk* tail_ = nullptr;
    std::uint64_t operation_count_ = 0u;
    std::uint64_t job_count_ = 0u;
    std::uint64_t dropped_operation_count_ = 0u;
    bool invalid_ = false;
    bool saturated_ = false;
    bool frozen_ = false;
};

static_assert(!std::is_copy_constructible_v<
    PersistedGpuOperationEvidenceLedger>);
static_assert(std::is_nothrow_move_constructible_v<
    PersistedGpuOperationEvidenceLedger>);

/// Convert the runtime-minted fixed GPU identity into the statement proof
/// without allocating or inferring cache state from descriptive counters.
/// Only the immutable all-hit policy accepted by the persisted operation can
/// become an all-GPU execution environment.
[[nodiscard]] inline std::optional<ExecutionEnvironmentProof>
make_persisted_shape_v5_gpu_environment_proof(
    const ::bioseq::gpu::vulkan::RuntimeGpuExecutionEnvironmentBinding&
        binding,
    const std::size_t effective_cpu_workers) noexcept {
    using RuntimePolicy = ::bioseq::gpu::vulkan::
        RuntimePipelineCachePolicyEvidence;
    if (!binding.accepted()
        || binding.cache_policy
            != RuntimePolicy::immutable_preseed_all_hit
        || effective_cpu_workers == 0
        || effective_cpu_workers > sql_integer_value_max
        || binding.seed_size > sql_bigint_value_max) {
        return std::nullopt;
    }
    ExecutionEnvironmentProof environment{};
    environment.effective_cpu_workers =
        static_cast<std::uint32_t>(effective_cpu_workers);
    environment.gpu_environment_present = binding.gpu_environment_present;
    environment.gpu_device_uuid = binding.gpu_device_uuid;
    environment.gpu_driver_identity = binding.gpu_driver_identity;
    environment.gpu_pipeline_cache_policy =
        PipelineCachePolicy::immutable_preseed_all_hit;
    environment.gpu_pipeline_cache_seed_sha256 = binding.seed_sha256;
    environment.gpu_pipeline_cache_seed_size = binding.seed_size;
    environment.gpu_pipeline_cache_manifest_checksum =
        binding.manifest_checksum;
    environment.gpu_pipeline_cache_hit = binding.pipeline_cache_all_hit
        ? OptionalProofBool::true_value
        : OptionalProofBool::false_value;
    return environment_is_valid_for_backend(
            environment, ExecutedBackend::vulkan_gpu)
        ? std::optional<ExecutionEnvironmentProof>{environment}
        : std::nullopt;
}

/// Convert the separately-owned report-once cache token into an operational
/// delta.  The descriptive environment proof is intentionally insufficient to
/// manufacture these counters, and the token identity itself is not replayed
/// into the SQL aggregate.
[[nodiscard]] inline std::optional<PipelineOperationalStats>
make_persisted_shape_v5_gpu_cache_metrics_delta(
    const ::bioseq::gpu::vulkan::RuntimeGpuCacheMetricsAudit& audit) noexcept {
    if (!audit.accepted()
        || audit.gpu_pipeline_cache_loads > sql_bigint_value_max
        || audit.gpu_pipeline_cache_seed_bytes > sql_bigint_value_max
        || audit.gpu_pipeline_cache_load_ns > sql_bigint_value_max
        || audit.gpu_pipeline_creates > sql_bigint_value_max
        || audit.gpu_pipeline_creation_ns > sql_bigint_value_max) {
        return std::nullopt;
    }
    PipelineOperationalStats delta{};
    delta.gpu_pipeline_cache_loads = audit.gpu_pipeline_cache_loads;
    delta.gpu_pipeline_cache_seed_bytes =
        audit.gpu_pipeline_cache_seed_bytes;
    delta.gpu_pipeline_cache_load_ns = audit.gpu_pipeline_cache_load_ns;
    delta.gpu_pipeline_creates = audit.gpu_pipeline_creates;
    delta.gpu_pipeline_creation_ns = audit.gpu_pipeline_creation_ns;
    return delta;
}

/// The runtime's sealed proof exposes an immutable environment preview before
/// it commits report-once cache bits.  Final authority must reproduce every
/// fixed field exactly; descriptive equivalence is not enough.
[[nodiscard]] inline bool
persisted_shape_v5_gpu_environment_binding_matches(
    const ::bioseq::gpu::vulkan::RuntimeGpuExecutionEnvironmentBinding& actual,
    const ::bioseq::gpu::vulkan::RuntimeGpuExecutionEnvironmentBinding&
        preview) noexcept {
    return actual.accepted() && preview.accepted()
        && actual == preview;
}

/// The pre-finalizer cache audit is a componentwise maximum claim, not the
/// counter delta to publish.  An accepted final report-once token may publish
/// only the same operation identity and values bounded by that validated
/// maximum.
[[nodiscard]] inline bool
persisted_shape_v5_gpu_cache_metrics_bounded_by(
    const ::bioseq::gpu::vulkan::RuntimeGpuCacheMetricsAudit& actual,
    const ::bioseq::gpu::vulkan::RuntimeGpuCacheMetricsAudit& preview) noexcept {
    return actual.componentwise_bounded_by(preview);
}

/// Validate a staged persisted-GPU stats delta and the prospective aggregate
/// without mutating the caller.  The scheduler performs this check before the
/// allocation-free commit so environment or persisted-generation drift cannot
/// revoke earlier jobs after any result/source/lifecycle state is published.
[[nodiscard]] constexpr bool
persisted_shape_v5_coordinate_prospective_stats_valid(
    const PipelineExecutionStats& before,
    const PipelineExecutionStats& staged,
    const PipelineExecutionStats& prospective,
    const std::uint64_t expected_jobs) noexcept {
    if (expected_jobs == 0 || expected_jobs > sql_bigint_value_max
        || before.attempted_alignment_jobs
            > sql_bigint_value_max - expected_jobs) {
        return false;
    }
    const std::uint64_t expected_total =
        before.attempted_alignment_jobs + expected_jobs;
    return PipelineExecutionStats::can_retain_full_attestation(before)
        && PipelineExecutionStats::can_retain_full_attestation(staged)
        && PipelineExecutionStats::can_retain_full_attestation(prospective)
        && staged.requested_backend == ExecutedBackend::vulkan_gpu
        && staged.actual_backend == ExecutedBackend::vulkan_gpu
        && staged.requested_target_index_source
            == TargetIndexSource::persisted
        && staged.actual_target_index_source == TargetIndexSource::persisted
        && staged.attempted_alignment_jobs == expected_jobs
        && staged.fully_attested_alignment_jobs == expected_jobs
        && staged.unattested_alignment_jobs == 0
        && prospective.attempted_alignment_jobs == expected_total
        && prospective.fully_attested_alignment_jobs == expected_total
        && prospective.unattested_alignment_jobs == 0
        && prospective.requested_backend == ExecutedBackend::vulkan_gpu
        && prospective.actual_backend == ExecutedBackend::vulkan_gpu
        && prospective.requested_target_index_source
            == TargetIndexSource::persisted
        && prospective.actual_target_index_source
            == TargetIndexSource::persisted
        && prospective.persisted_generation_id
            == staged.persisted_generation_id
        && prospective.persisted_source_identity
            == staged.persisted_source_identity
        && ExecutionEnvironmentProof::payload_equal(
            prospective.environment, staged.environment)
        && !prospective.environment.mixed_invalid;
}

} // namespace bioseq::pipeline_stats

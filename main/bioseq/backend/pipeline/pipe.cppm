/**
 * @file pipe.cppm
 * @brief Templated minimap_port pipeline API v2 (DOC0249).
 *
 * This module implements the constexpr-dispatched stage pipeline described in
 * `TEMPLATED_MINIMAP_PORT_PIPELINE_API_V2_CONSTEXPR_DISPATCHED_STAGES_AND_METADATA__DOC0249.md`.
 * It is a parallel API layer that sits above the existing kernel functions in
 * `bioseq::simd::active_target` and provides:
 *
 *   - Compile-time parameter packs (`ConstexprAlignerParams<W,K,L>` /
 *     `ConstexprAlignerFallbackParams<L>`).
 *   - Stage skeletons (`BareStage<Self>`, `StageBase<Self,Out>`,
 *     `Stage<Cep,Self,Sink>`).
 *   - `make_pipeline<Cep>` variadic composer.
 *   - `BatchT<E,StorageKind>` storage wrappers backed by `std::span` or the
 *     per-pipeline arena context.
 *   - Concrete stages — every pipeline phase exposed as a stand-alone struct
 *     whose `process<Cep>` body forwards to the existing kernel function
 *     (collect_seed_hits, chain_anchors_*, generate_regions, etc.).
 *   - Top-level `dispatch_pipeline(...)` cascade that fixes (Cep, BAA, CA,
 *     MA, EP) at compile time before driving the pipeline.
 *   - `PipelineDescription` metadata surface for the Postgres EXPLAIN
 *     renderer.
 *
 * The pipeline calls into the existing kernels directly; no kernel rename is
 * performed here.  Stages adapt span-based I/O to the kernel's `std::vector`
 * signatures by materialising the kernel's output into arena-backed slabs
 * after the kernel returns.  The wire type between stages is `std::span<E>`
 * (with mutating stages taking non-const elements and read-only stages
 * taking const).
 */
module;

#include "headers/pg_build_config.h"

// DOC0256 §R17 — the chain primitive (`pipe::detail::dispatch_chain`) relies
// on tree-inlining of [[gnu::always_inline]] recursive instantiations.  Both
// gcc and clang honour the attribute at -O1+, but downgrade silently at -O0.
// Build configs that compile this module without optimisation produce
// stack-spilled adaptor frames that defeat the point of the cascade; refuse
// to compile in that mode.
#if !defined(__OPTIMIZE__)
# error "DOC0256 chain primitive requires -O1 or higher (see DOC0256 §R17). " \
        "Use -Og or -O1 -g for debug builds of pipe.cppm."
#endif

#ifndef BIOSEQ_PIPELINE_FORCE_INLINE
#define BIOSEQ_PIPELINE_FORCE_INLINE 1
#endif

#if BIOSEQ_PIPELINE_FORCE_INLINE
#define BIOSEQ_PIPELINE_INLINE_ATTR [[gnu::always_inline]]
#define BIOSEQ_PIPELINE_INLINE_FLATTEN_ATTR [[gnu::always_inline, gnu::flatten]]
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR [[nodiscard, gnu::always_inline]]
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR [[nodiscard, gnu::always_inline, gnu::flatten]]
#else
#define BIOSEQ_PIPELINE_INLINE_ATTR
#define BIOSEQ_PIPELINE_INLINE_FLATTEN_ATTR
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR [[nodiscard]]
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR [[nodiscard]]
#endif

#include <hwy/highway.h>

#include "common.h"

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <algorithm>
#include <atomic>
#include <array>
#include <concepts>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>

export module bioseq.sequences.pipe;

import bioseq.common.mlogging;
import bioseq.common.templating;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.two_stacks;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.lane_helpers;
import bioseq.alignment.ksw2_batched;
import bioseq.alignment.block_aligner;
import bioseq.sequences.chaining.utils;
import bioseq.sequences.chaining.scalar;
import bioseq.sequences.chaining.simd;
import bioseq.sequences.chaining.rmq;
import bioseq.sequences.chaining.vulkan;
import bioseq.sequences.minimap_port;
import bioseq.sequences.prefilters.prefilter_types;
import bioseq.sequences.prefilters.sneaky_snake;
import bioseq.sequences.prefilters.sneaky_snake_vulkan;
import bioseq.sequences.prefilters.gatekeeper;
import bioseq.sequences.prefilters.gatekeeper_vulkan;
import bioseq.common.context;
import bioseq.instr;
import bioseq.storage.scanner;
import bioseq.storage.lazy;
import bioseq.storage.size;   // DOC0263 §10 — sequence_size_t for the C1 truncation audit
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;   // DOC0279 §9 — relocated MinimizerHashmapFragmentIndex (was TargetIndex)
import bioseq.indexing.minimizer_index;     // DOC0279 §7.1 — relocated MinimizerIndex concept
import bioseq.gpu.vulkan.runtime;           // Runtime::instance() for GPU sketch/seed paths
import bioseq.sequences.kmers.vulkan;       // DOC0351 Phase C — Vulkan minimizer sketch + device span
import bioseq.indexing.minimizer_vulkan;    // DOC0307 Phase B — GPU-resident index view + cache
import bioseq.sequences.seeding.vulkan;     // DOC0307 Phase B — Vulkan seed-hit lookup
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

// DOC0371 §5.1 — one target-independent execution-attestation value.  This
// namespace deliberately precedes Highway target toggling: these types describe
// a whole statement and must have one ABI regardless of the compiled SIMD
// target.  Algorithm-local proof values are mapped into this model at dispatch
// boundaries; no Highway namespace owns an alias of it.
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
            || !payload_equal(*this, rhs)) {
            mixed_invalid = true;
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
        || environment.gpu_pipeline_cache_seed_size > sql_bigint_value_max) {
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
                && (!ExecutionEnvironmentProof::payload_equal(
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
            && !ExecutionEnvironmentProof::payload_equal(
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
            && persisted_identity_compatible;

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

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE::pipe {

namespace prefilters = ::bioseq::sequences::prefilters;
// DOC0279 §9 — bring the relocated index into this namespace so the prior
// `TargetIndex` spellings resolve to `MinimizerHashmapFragmentIndex`.
using bioseq::indexing::MinimizerHashmapFragmentIndex;
using bioseq::indexing::MinimizerLookupIndex;
using bioseq::indexing::MinimizerSeedIndex;

// Export exception aliases through the pipeline module so PG importers do not
// redundantly import the Highway-heavy implementation modules.
using ExactSimdRuntimeError =
    ::bioseq::simd::HWY_NAMESPACE::ExactSimdRuntimeError;
using SimdChainingRuntimeError =
    ::bioseq::simd::HWY_NAMESPACE::SimdChainingRuntimeError;
using SimdChainingStatus =
    ::bioseq::simd::HWY_NAMESPACE::SimdChainingStatus;

/// Fail-closed error for the selected Paper-ntHash route.  PostgreSQL-facing
/// callers catch this concrete type before entering the longjmp ERROR boundary;
/// no Paper rejection is allowed to escape the C++ frame or select another
/// minimizer implementation.
class PaperNtHashRuntimeError final : public std::runtime_error {
public:
    explicit PaperNtHashRuntimeError(std::string detail)
        : std::runtime_error(
              std::string{
                  "PaperNtHashSimd minimizer execution rejected: "}
              + std::move(detail)) {}
};

// DOC0263 §10.1 — pipeline lengths/positions use the 64-bit sequence_size_t.
using bioseq::storage::sequence_size_t;

using HostStageClock = std::chrono::steady_clock;

[[nodiscard]] inline std::uint64_t host_stage_now_ns() noexcept {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        HostStageClock::now().time_since_epoch()).count();
    return now > 0 ? static_cast<std::uint64_t>(now) : 1u;
}

[[nodiscard]] inline std::uint64_t elapsed_host_stage_ns(
        const std::uint64_t started_ns) noexcept {
    const std::uint64_t finished_ns = host_stage_now_ns();
    const std::uint64_t elapsed = finished_ns >= started_ns
        ? finished_ns - started_ns : 0u;
    // A stage that executed is positive evidence even on a platform whose
    // steady-clock tick is coarser than one nanosecond. The clamp can add at
    // most one nanosecond and keeps focused tests independent of clock
    // resolution without sleeping.
    return elapsed > 0 ? elapsed : 1u;
}

[[noreturn]] inline void throw_vulkan_declined(std::string_view stage) {
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    const std::string detail = !runtime.usable()
        ? fmt::format("runtime unavailable: {}", runtime.initialization_error())
        : runtime.features().software_device
            ? fmt::format(
                  "active Vulkan device '{}' is a CPU/software device "
                  "(VkPhysicalDeviceType={}); explicit GPU execution requires a hardware Vulkan device",
                  runtime.features().device_name.data(),
                  runtime.features().device_type)
            : "GPU path declined";
    throw bioseq::gpu::vulkan::VulkanRuntimeError(
        fmt::format("{} selected but Vulkan execution did not run ({})", stage, detail));
}

[[noreturn]] inline void throw_vulkan_sketch_declined(
    std::string_view stage,
    const bioseq::gpu::vulkan::VulkanSketchDecline& decline)
{
    if (!decline.declined()) {
        throw_vulkan_declined(stage);
    }
    bioseq::gpu::vulkan::throw_vulkan_strict_decline(stage, decline);
}

// DOC0256 §2.1 — the chain primitive is load-bearing on relaxed
// template-template parameter matching (P0522R0) and C++20 class-type
// NTTPs (carrying `auto...` packs that interleave enum NTTPs with
// `std::size_t`).  Guard the build mode so a misconfigured build fails
// loudly rather than producing subtly-wrong codegen.
static_assert(__cplusplus >= 202302L,
    "DOC0256 requires C++23 — see DOC0256 §2.1.");

// Import types from the surrounding modules so the stage bodies can refer
// to them by short name.
using ::bioseq::chaining::Anchor;
using ::bioseq::chaining::Chain;
using ::bioseq::chaining::ChainingParams;
using ::bioseq::chaining::ArenaSlab;
using ::bioseq::chaining::make_arena_slab;
using ::bioseq::chaining::chain_anchors_scalar;
using ::bioseq::chaining::chain_anchors_rmq;

// =============================================================================
// 4. Compile-time parameter packs (DOC0249 §4)
// =============================================================================

/**
 * @brief Type-level parameter pack for a fully specialised pipeline instance.
 *
 * @tparam W           Minimizer window size (must be > 0).
 * @tparam K           K-mer length (must match the MinimizerRepr's KmerLen).
 * @tparam SeqLenLog2  ceil(log2(max(query_bp, target_bp))).  Selects the
 *                     `MinimizerRepr<SeqLenLog2, K>` storage bucket — same
 *                     dispatch already happens at runtime in `minimap_port`.
 */
template <std::size_t W, std::size_t K, std::size_t SeqLenLog2>
struct ConstexprAlignerParams {
    static constexpr std::size_t window_len        = W;
    static constexpr std::size_t kmer_len          = K;
    static constexpr std::size_t sequence_len_log2 = SeqLenLog2;
    static constexpr bool        generic_fallback  = false;

    using minimizer_repr_t = ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<SeqLenLog2, K>;

    [[nodiscard]] static constexpr std::string_view label() noexcept {
        return "specialised";
    }
};

/**
 * @brief Fallback parameter pack for non-canonical (W, K).
 *
 * Stages branch on `generic_fallback` to read window/kmer from runtime params
 * instead of the type-level constants.
 */
template <std::size_t SeqLenLog2>
struct ConstexprAlignerFallbackParams {
    static constexpr std::size_t window_len        = 0;
    static constexpr std::size_t kmer_len          = 0;
    static constexpr std::size_t sequence_len_log2 = SeqLenLog2;
    static constexpr bool        generic_fallback  = true;

    using minimizer_repr_t = ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<SeqLenLog2, 15u>;

    [[nodiscard]] static constexpr std::string_view label() noexcept {
        return "fallback(generic w/k)";
    }
};

template <class T>
concept is_constexpr_aligner_params = requires {
    { T::window_len }        -> std::convertible_to<std::size_t>;
    { T::kmer_len }          -> std::convertible_to<std::size_t>;
    { T::sequence_len_log2 } -> std::convertible_to<std::size_t>;
    { T::generic_fallback }  -> std::convertible_to<bool>;
    typename T::minimizer_repr_t;
    { T::label() }           -> std::convertible_to<std::string_view>;
};

static_assert(is_constexpr_aligner_params<ConstexprAlignerParams<10u, 15u, 28u>>);
static_assert(is_constexpr_aligner_params<ConstexprAlignerFallbackParams<28u>>);

/**
 * @brief Pipeline-builder entry-point tag (DOC0249 §9.4).
 *
 * `WithPrebuiltMinimizers` — pipeline accepts a `std::span<const MinimizerT>`
 * directly; the IndexStage is at the head.
 * `FromSequence` — pipeline accepts the query sequence range and a
 * `MinimizerStage` is front-loaded to sketch on the fly.
 */
enum class EntryPoint : std::uint8_t {
    WithPrebuiltMinimizers,
    FromSequence,
};

// =============================================================================
// 5/6. Concepts and Stage skeleton (DOC0249 §5, §6)
// =============================================================================

namespace detail {
    template <class S>
    concept IsBareStage = requires { typename S::bare_stage_marker; };
}

/**
 * @brief Base tag inherited by every concrete stage.
 *
 * Lets `make_pipeline` distinguish bare-stage arguments from the terminal
 * sink via the `bare_stage_marker` typedef.
 */
template <class Derived>
struct BareStage {
    using bare_stage_marker = void;
};

// Forward declaration so StageBase::attach can refer to Stage.
template <is_constexpr_aligner_params Cep, class Self, class Sink>
struct Stage;

// Metadata types (defined fully in §11; forward-declared here so describe()
// has a return type).
struct StageDescription;
struct PipelineDescription;

// =============================================================================
// DOC0307 §5.2 — narrow minimizer-batch shape carrying an optional device span.
// =============================================================================
//
// One additional stage-shape on top of the DOC0254 matrix. It borrows the host
// minimizers from the arena slab and owns the optional GPU device minimizer
// handle through `VulkanDeviceMinimizerSpan::owner`, so the all-Vulkan
// sketch+seed path avoids a second query-minimizer upload. CPU minimizer
// kernels keep using the existing `std::span<const MinimizerT>` path.
template <class MinimizerT>
struct MinimizerBatchView {
    using value_type = MinimizerT;

    std::span<const MinimizerT> host{};
    std::optional<bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>> device{};

    [[nodiscard]] const MinimizerT* data() const noexcept { return host.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return host.size(); }
    [[nodiscard]] std::span<const MinimizerT> as_span() const noexcept { return host; }
    [[nodiscard]] auto begin() const noexcept { return host.begin(); }
    [[nodiscard]] auto end() const noexcept { return host.end(); }
};

template <class T>
struct is_minimizer_batch_view : std::false_type {};
template <class M>
struct is_minimizer_batch_view<MinimizerBatchView<M>> : std::true_type {};
template <class T>
inline constexpr bool is_minimizer_batch_view_v =
    is_minimizer_batch_view<std::remove_cvref_t<T>>::value;

// =============================================================================
// DOC0254 §7 — Concept-driven sink validation.
// =============================================================================

/**
 * @brief Single-batch sink concept.  Required by EmitToken::emit_batch.
 *
 * Matched by every concrete sink that consumes a `std::span<E>` (most
 * pipeline tails).  Substitution failures inside `EmitToken::emit_batch`
 * name the concept and the missing method.
 */
template <class S, class E>
concept BatchSink = requires (S& sink, std::span<E> batch) {
    { sink.on_batch(batch) } -> std::same_as<void>;
    { sink.on_completed() }  -> std::same_as<void>;
};

/**
 * @brief Pair-batch sink concept.  Required by EmitToken::emit_batch_pair.
 *
 * The pair shape is used by the chain → region → primary/secondary →
 * filter → divergence → align cascade where each stage forwards both the
 * regions span and the (read-only) anchors span downstream.
 */
template <class S, class E1, class E2>
concept PairBatchSink = requires (S& sink, std::span<E1> a, std::span<E2> b) {
    { sink.on_batch_pair(a, b) } -> std::same_as<void>;
    { sink.on_completed() }      -> std::same_as<void>;
};

/**
 * @brief Index-decorated batch sink concept.  Required by
 * EmitToken::emit_batch_with_index.
 *
 * Used by IndexStage to hand a sketches span downstream alongside a
 * (const-reference) target index.  SeedHitStage is the canonical consumer.
 */
template <class S, class E, class IndexT>
concept IndexedBatchSink = requires (S& sink, std::span<E> a, const IndexT& idx,
                                     bioseq::indexing::GpuIndexCacheIdentity identity) {
    { sink.on_batch_with_index(a, idx, identity) } -> std::same_as<void>;
    { sink.on_completed() }                        -> std::same_as<void>;
};

/// DOC0307 §5.2 — minimizer-batch sink (MinimizerStage → IndexStage hand-off).
template <class S, class MinimizerT>
concept MinimizerBatchSink = requires (S& sink, MinimizerBatchView<MinimizerT> batch) {
    { sink.on_minimizer_batch(batch) } -> std::same_as<void>;
    { sink.on_completed() }            -> std::same_as<void>;
};

/// DOC0307 §5.2 — index-decorated minimizer-batch sink (IndexStage →
/// SeedHitStage hand-off carrying the GPU index cache identity).
template <class S, class MinimizerT, class IndexT>
concept IndexedMinimizerBatchSink =
requires (S& sink, MinimizerBatchView<MinimizerT> batch, const IndexT& idx,
          bioseq::indexing::GpuIndexCacheIdentity identity) {
    { sink.on_minimizer_batch_with_index(batch, idx, identity) } -> std::same_as<void>;
    { sink.on_completed() }                                      -> std::same_as<void>;
};

// =============================================================================
// DOC0254 §4 — Compile-time single-emission enforcement (EmitToken /
// EmittedToken).
// =============================================================================

template <class Sink> class EmittedToken;

/**
 * @brief Move-only, single-use, friend-constructible-only token granting
 * downstream-emission permission.  The `emit_batch*` family of
 * rvalue-qualified methods consumes the token (nulling its internal
 * pointer) and returns the friend-only-constructible `EmittedToken<Sink>`
 * proof.
 *
 * The token's only constructor is private; only `Stage<Cep, Self, Sink>`
 * is befriended, which means concrete-stage `process*<Cep>` bodies see
 * the token by rvalue reference but cannot manufacture one themselves —
 * the type system rules out skipping the emit (no way to construct the
 * required `EmittedToken<Sink>` return value).
 *
 * Single-emission enforcement (DOC0254 R7):
 *   - At least once: HARD compile-time.  `process*<Cep>` must return
 *     `EmittedToken<Sink>`; the only way to construct one is to call
 *     `EmitToken<Sink>::emit_batch*`.
 *   - At most once: soft compile-time + runtime trip-wire.  `[[nodiscard]]`
 *     makes the ergonomic shape `return std::move(token).emit_batch*(...)`;
 *     emitting twice trips the `DEBUG_ASSERT(sink_ptr_ != nullptr)` in debug.
 */
template <class Sink>
class EmitToken {
public:
    template <is_constexpr_aligner_params, class, class> friend struct Stage;

    EmitToken()                            = delete;
    EmitToken(const EmitToken&)            = delete;
    EmitToken(EmitToken&&)                 = delete;
    EmitToken& operator=(const EmitToken&) = delete;
    EmitToken& operator=(EmitToken&&)      = delete;

    template <class E>
    [[nodiscard, gnu::always_inline]]
    EmittedToken<Sink> emit_batch(std::span<E> data) && {
        static_assert(BatchSink<Sink, E>,
            "EmitToken::emit_batch: downstream sink does not implement "
            "on_batch(std::span<E>). See DOC0249 §8.12 stage-shape matrix.");
        DEBUG_ASSERT(sink_ptr_ != nullptr,
            "EmitToken consumed twice; the first emit_batch* call nulled the "
            "sink pointer (see R7 in DOC0254 §3).");
        sink_ptr_->on_batch(data);
        sink_ptr_ = nullptr;
        return EmittedToken<Sink>::make();
    }

    template <class E1, class E2>
    [[nodiscard, gnu::always_inline]]
    EmittedToken<Sink> emit_batch_pair(std::span<E1> first_batch,
                                       std::span<E2> second_batch) && {
        static_assert(PairBatchSink<Sink, E1, E2>,
            "EmitToken::emit_batch_pair: downstream sink does not implement "
            "on_batch_pair(std::span<E1>, std::span<E2>).");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "EmitToken consumed twice.");
        sink_ptr_->on_batch_pair(first_batch, second_batch);
        sink_ptr_ = nullptr;
        return EmittedToken<Sink>::make();
    }

    template <class E, class IndexT>
    [[nodiscard, gnu::always_inline]]
    EmittedToken<Sink> emit_batch_with_index(
        std::span<E>  data,
        const IndexT& index_ref,
        bioseq::indexing::GpuIndexCacheIdentity identity) && {
        static_assert(IndexedBatchSink<Sink, E, IndexT>,
            "EmitToken::emit_batch_with_index: downstream sink does not "
            "implement on_batch_with_index(std::span<E>, IndexT const&, identity).");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "EmitToken consumed twice.");
        sink_ptr_->on_batch_with_index(data, index_ref, identity);
        sink_ptr_ = nullptr;
        return EmittedToken<Sink>::make();
    }

    // DOC0307 §5.2 — narrow minimizer-batch emit (carries optional device span).
    template <class MinimizerT>
    [[nodiscard, gnu::always_inline]]
    EmittedToken<Sink> emit_minimizer_batch(MinimizerBatchView<MinimizerT> batch) && {
        static_assert(MinimizerBatchSink<Sink, MinimizerT>,
            "EmitToken::emit_minimizer_batch: downstream sink does not implement "
            "on_minimizer_batch.");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "EmitToken consumed twice.");
        sink_ptr_->on_minimizer_batch(batch);
        sink_ptr_ = nullptr;
        return EmittedToken<Sink>::make();
    }

    template <class MinimizerT, class IndexT>
    [[nodiscard, gnu::always_inline]]
    EmittedToken<Sink> emit_minimizer_batch_with_index(
        MinimizerBatchView<MinimizerT> batch,
        const IndexT& index_ref,
        bioseq::indexing::GpuIndexCacheIdentity identity) && {
        static_assert(IndexedMinimizerBatchSink<Sink, MinimizerT, IndexT>,
            "EmitToken::emit_minimizer_batch_with_index: downstream sink does not "
            "implement on_minimizer_batch_with_index.");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "EmitToken consumed twice.");
        sink_ptr_->on_minimizer_batch_with_index(batch, index_ref, identity);
        sink_ptr_ = nullptr;
        return EmittedToken<Sink>::make();
    }

private:
    Sink* sink_ptr_;
    explicit constexpr EmitToken(Sink& sink_ref) noexcept
        : sink_ptr_(&sink_ref) {}
};

/**
 * @brief Proof-of-emission token.  Returned by
 * `EmitToken<Sink>::emit_batch*`; every concrete stage's `process*<Cep>`
 * must return one.  The only path to constructing one is the
 * friend-accessible `make()` factory.
 */
template <class Sink>
class [[nodiscard]] EmittedToken {
public:
    EmittedToken(const EmittedToken&)             = delete;
    EmittedToken& operator=(const EmittedToken&)  = delete;
    EmittedToken& operator=(EmittedToken&&)       = delete;

    /// Move constructor exists only for mandatory-copy-elision worst-case
    /// materialisation.  The canonical path is direct-return from
    /// emit_batch*, where C++17 copy elision skips it entirely.
    EmittedToken(EmittedToken&&) noexcept = default;

private:
    friend class EmitToken<Sink>;
    /// Friend-only default constructor — the public surface is
    /// non-default-constructible because the only call site of this
    /// ctor lives behind the `make()` factory below.
    constexpr EmittedToken() noexcept = default;
    [[nodiscard, gnu::always_inline]] static constexpr EmittedToken make() noexcept {
        return EmittedToken{};
    }
};

// =============================================================================
// DOC0254 §5 — Input-side kernel-to-arena adapter.
// =============================================================================

/**
 * @brief Drop-in `sketch_collector_t<E>`-shaped adapter that routes the
 * kernel's `collect(...)` traffic into a caller-provided
 * `ArenaSlab<E>`.  Implements the minimal surface the kernels rely on
 * (`collect(E&&)`, `collect(const E&)`, `reserve(size_t)`).
 *
 * `ArenaSlab<E>::push_back` requires `std::is_trivially_copyable_v<E>` —
 * satisfied by every `MinimizerRepr<L, K>`.  The existing slab
 * `static_assert` (`chaining_utils.cppm:236`) covers the constraint and
 * would fire if a non-trivial minimizer type were ever introduced.
 */
template <class E>
struct sketch_collector_arena_adapter {
    using minimizer_type = E;

    ::bioseq::chaining::ArenaSlab<E>* slab_;

    [[gnu::always_inline]] void collect(E sketch) {
        slab_->push_back(std::move(sketch));
    }
    [[gnu::always_inline]] void collect(const E& sketch) {
        slab_->push_back(sketch);
    }
    [[gnu::always_inline]] void reserve(std::size_t expected_count) {
        slab_->reserve(expected_count);
    }
    [[nodiscard, gnu::always_inline]] std::size_t size() const noexcept {
        return slab_->size();
    }

    template<class M = E>
    [[nodiscard, gnu::always_inline]] bool append_raw_storage(
        const typename M::storage_t* raw,
        std::size_t count)
        requires (
            std::same_as<M, E>
            && requires { typename M::storage_t; }
            && std::is_trivially_copyable_v<M>
            && sizeof(M) == sizeof(typename M::storage_t))
    {
        const std::size_t old_size = slab_->size();
        if (count > (std::numeric_limits<std::size_t>::max)() - old_size) {
            return false;
        }
        // `estimated_sketch_count()` is intentionally only an estimate.  XXL
        // inputs commonly exceed it by many raw-output batches; exact growth
        // here caused one full-prefix copy and one unreclaimable arena block per
        // batch.  Geometric growth bounds both copies and live arena footprint.
        slab_->reserve_geometric(old_size + count);
        if (count != 0) {
            std::memcpy(
                static_cast<void*>(slab_->data() + old_size),
                static_cast<const void*>(raw),
                count * sizeof(typename M::storage_t));
        }
        slab_->resize(old_size + count);
        return true;
    }
};

/**
 * @brief CRTP helper absorbing the boilerplate shared by every concrete stage.
 *
 * `Derived` must define `static constexpr std::string_view stage_name`.  The
 * default `attach<Cep, Sink>(...)` returns a bound `Stage<Cep, Derived, Sink>`
 * value owning `*this` and the downstream sink.
 */
template <class Derived, class Out>
struct StageBase : BareStage<Derived> {
    using out_element_type = Out;

    template <is_constexpr_aligner_params Cep, class Sink>
    [[gnu::always_inline]] auto attach(Sink sink) noexcept {
        return Stage<Cep, Derived, Sink>{
            std::move(sink), std::move(static_cast<Derived&>(*this))};
    }

    template <class Sink>
    [[gnu::always_inline]] void finalize(Sink&) noexcept {
        /* default: no-op */
    }
};

/**
 * @brief Bound stage — owns its downstream sink and bare-stage config by value.
 *
 * `Cep` is fixed at `make_pipeline<Cep>(...)` time and identical across every
 * stage in a given pipeline instance.  The bound stage's `on_batch*` members
 * forward to the bare stage's `process*<Cep>` body; both are
 * `[[gnu::always_inline]]` so the chain collapses to a straight-line sequence
 * of kernel calls at `-O3`.
 */
template <is_constexpr_aligner_params Cep, class Self, class Sink>
struct Stage {
    using cep_t = Cep;

    Sink sink_;
    Self self_;

    constexpr Stage(Sink sink_arg, Self self_arg) noexcept
        : sink_(std::move(sink_arg)), self_(std::move(self_arg)) {}

    // DOC0254 §4.2: the bound stage constructs an `EmitToken<Sink>` on its
    // own stack and hands it to `process*<Cep>` by rvalue reference; the
    // concrete-stage body returns the (discarded) `EmittedToken<Sink>` proof
    // type whose construction is friend-private to `EmitToken<Sink>`.
    template <class B>
    [[gnu::always_inline]] void on_batch(B&& batch) {
        (void) self_.template process<Cep>(
            std::forward<B>(batch), EmitToken<Sink>{sink_});
    }

    template <class B1, class B2>
    [[gnu::always_inline]] void on_batch_pair(B1&& first_batch, B2&& second_batch) {
        (void) self_.template process_pair<Cep>(
            std::forward<B1>(first_batch), std::forward<B2>(second_batch),
            EmitToken<Sink>{sink_});
    }

    template <class B, class IndexT>
    [[gnu::always_inline]] void on_batch_with_index(
        B&& batch, const IndexT& index_ref,
        bioseq::indexing::GpuIndexCacheIdentity identity) {
        (void) self_.template process_with_index<Cep>(
            std::forward<B>(batch), index_ref, identity, EmitToken<Sink>{sink_});
    }

    // DOC0307 §5.2 — forwarders for the narrow minimizer-batch shape.
    template <class MinimizerT>
    [[gnu::always_inline]] void on_minimizer_batch(MinimizerBatchView<MinimizerT> batch) {
        (void) self_.template process<Cep>(batch, EmitToken<Sink>{sink_});
    }

    template <class MinimizerT, class IndexT>
    [[gnu::always_inline]] void on_minimizer_batch_with_index(
        MinimizerBatchView<MinimizerT> batch, const IndexT& index_ref,
        bioseq::indexing::GpuIndexCacheIdentity identity) {
        (void) self_.template process_with_index<Cep>(
            batch, index_ref, identity, EmitToken<Sink>{sink_});
    }

    [[gnu::always_inline]] void on_completed() noexcept {
        self_.finalize(sink_);
        sink_.on_completed();
    }

    [[nodiscard]] PipelineDescription describe() const;
};

/**
 * @brief Variadic pipeline composer — fixes `Cep` once for the whole chain.
 *
 * The base case (terminal sink) returns the sink unchanged; the recursive case
 * peels one bare stage off the front, builds the downstream pipeline from the
 * remaining arguments, and attaches.
 */
template <is_constexpr_aligner_params Cep, class Sink>
[[gnu::always_inline]] constexpr auto make_pipeline(Sink sink) noexcept {
    static_assert(!detail::IsBareStage<Sink>,
        "make_pipeline's last argument must be a terminal sink, "
        "not a BareStage.  Append your terminal sink at the end.");
    return sink;
}

template <is_constexpr_aligner_params Cep, class Bare0, class... Rest>
    requires detail::IsBareStage<Bare0>
[[gnu::always_inline]] constexpr auto make_pipeline(Bare0 head, Rest... rest) noexcept {
    auto downstream = make_pipeline<Cep>(std::move(rest)...);
    return std::move(head).template attach<Cep>(std::move(downstream));
}

// =============================================================================
// 7. Batch storage — owning arena slab (DOC0249 §7)
// =============================================================================
//
// `ArenaSlab<E>` itself lives in `bioseq_sequences.chaining.utils` so the
// chaining kernels (chain_anchors_*) and the seed/region kernels in
// `minimap_port.cppm` can take it as an output parameter — see DOC0249 §13
// follow-up "output-side kernel-to-arena refactor".  The `using` declarations
// at the top of this namespace re-export both the slab type and the
// `make_arena_slab<E>()` helper so stage bodies refer to them by short name.

// =============================================================================
// 11. Metadata surface (DOC0249 §11) — forward types for algorithm_traits
// =============================================================================
//
// The full `StageDescription` / `PipelineDescription` definitions live after
// the algorithm_traits block (they reference `NamedProperty` too).  The
// `NamedProperty` POD itself is the only piece the traits' `computed_properties_default()`
// signature needs, so we hoist it here ahead of the traits.

struct NamedProperty {
    std::string name;
    std::string value;
};

// =============================================================================
// 8.0 Kernel-trait pattern (DOC0249 §8.0, R5)
// =============================================================================
//
// `algorithm_traits<AlgoEnumValue>` maps an algorithm enum value to
//   - a `kernel_type` struct exposing the call surface used by the stage,
//   - a `static constexpr std::string_view algorithm_label()` for EXPLAIN,
//   - a `static std::vector<NamedProperty> computed_properties_default()`
//     for the EXPLAIN renderer's `[name: value]` properties.
//
// Stages take the `kernel_type` as a template argument (via `kernel_t<X>`)
// and call `Kernel::call(...)` once — no `if constexpr` ladders inside the
// stage body.  Adding a new algorithm is a single trait specialisation in
// the kernel's own module.

/// Dependent-false helper so the primary template's `static_assert` fires
/// only on instantiation (not at point of definition).
template <auto>
inline constexpr bool no_kernel_for_v = false;

/// Primary template — unspecialised.  Substitution failure here produces a
/// readable diagnostic naming the offending enum value.
///
/// Specialisations live below for `ChainingAlgorithmType` and
/// `MinimizerAlgorithmType`; `BaseAlignmentAlgorithmType` is still dispatched
/// inside `AlignStage::process_pair` via `align_regions<BAA>` because that
/// kernel's signature is templated on both `BAA` and the per-stage sequence
/// ranges `(QR, TR)` — a trait specialisation would still need to forward
/// `QR`/`TR`, so the direct call is preferred for clarity.  A future
/// iteration can move `AlignStage` to the trait pattern once the kernel
/// surface is refactored to take a uniform-shape output adapter.
template <auto AlgoEnumValue>
struct algorithm_traits {
    static_assert(no_kernel_for_v<AlgoEnumValue>,
        "algorithm_traits<X>: no specialisation for this algorithm enum value. "
        "Declare one in pipe.cppm next to the ChainingAlgorithmType / "
        "MinimizerAlgorithmType blocks.");
};

/// Helper alias — `kernel_t<X>` gives the kernel type directly.
template <auto AlgoEnumValue>
using kernel_t = typename algorithm_traits<AlgoEnumValue>::kernel_type;

// ---------------------------------------------------------------------------
// 8.0.1 ChainingAlgorithmType kernel traits
// ---------------------------------------------------------------------------

/// Scalar DP chaining — `chain_anchors_scalar` from
/// `bioseq_sequences.chaining.scalar`.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SCALAR_MM2> {
    struct kernel_type {
        static constexpr bool emits_scalar_chaining_execution_proof = true;
        static constexpr bool emits_simd_chaining_execution_proof = false;
        static constexpr bool emits_partial_vulkan_execution_proof = false;
        [[gnu::always_inline]] inline static void call(
            std::span<const Anchor> anchors,
            const ChainingParams&   params,
            ArenaSlab<Chain>&       chains_out,
            ArenaSlab<Anchor>&      reordered_anchors_out,
            ScalarPipelineExecutionProof* proof)
        {
            ::bioseq::chaining::chain_anchors_scalar(
                anchors, params, chains_out, reordered_anchors_out);
            if (proof != nullptr && !anchors.empty()) {
                proof->record_scalar_chain(
                    static_cast<std::uint64_t>(anchors.size()));
            }
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::chaining_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SCALAR_MM2);
        }
        static void populate_runtime_params(
            const ChainingParams&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "false"},
                {"complexity", "O(n * max_iter)"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// SIMD DP chaining — `chain_anchors_simd` from
/// `bioseq_sequences.chaining.simd` (lives in `bioseq::simd::HWY_NAMESPACE`).
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SIMD_MM2> {
    struct kernel_type {
        static constexpr bool emits_scalar_chaining_execution_proof = false;
        static constexpr bool emits_simd_chaining_execution_proof = true;
        static constexpr bool emits_partial_vulkan_execution_proof = false;
        [[gnu::always_inline]] inline static void call(
            std::span<const Anchor> anchors,
            const ChainingParams&   params,
            ArenaSlab<Chain>&       chains_out,
            ArenaSlab<Anchor>&      reordered_anchors_out,
            SimdChainingExecutionProof* proof)
        {
            ::bioseq::simd::HWY_NAMESPACE::chain_anchors_simd(
                anchors, params, chains_out, reordered_anchors_out,
                proof, /*aggregate_proof=*/true);
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::chaining_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SIMD_MM2);
        }
        static void populate_runtime_params(
            const ChainingParams& params,
            std::vector<NamedProperty>& properties) {
            properties.push_back({
                "simd_chaining_policy",
                std::string{::bioseq::chaining::simd_chaining_policy_name(
                    params.simd_policy)}});
        }
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "true"},
                {"requires_zero_skip_penalty", "false"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// RMQ tree-based chaining — `chain_anchors_rmq` from
/// `bioseq_sequences.chaining.rmq`.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::RMQ> {
    struct kernel_type {
        static constexpr bool emits_scalar_chaining_execution_proof = false;
        static constexpr bool emits_simd_chaining_execution_proof = false;
        static constexpr bool emits_partial_vulkan_execution_proof = false;
        [[gnu::always_inline]] inline static void call(
            std::span<const Anchor> anchors,
            const ChainingParams&   params,
            ArenaSlab<Chain>&       chains_out,
            ArenaSlab<Anchor>&      reordered_anchors_out)
        {
            ::bioseq::chaining::chain_anchors_rmq(
                anchors, params, chains_out, reordered_anchors_out);
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::chaining_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::RMQ);
        }
        static void populate_runtime_params(
            const ChainingParams&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "false"},
                {"complexity", "O(n log n)"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Vulkan tiled-DP chaining (DOC0304 Phase A) — `chain_anchors_vulkan` from
/// `bioseq_sequences.chaining.vulkan`. G3SA-style tiled DP score generation on
/// Vulkan + bioseq CPU `backtrack_chains`; selected Vulkan execution is
/// mandatory and failures throw `VulkanRuntimeError`.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::VULKAN_TILED_DP> {
    struct kernel_type {
        static constexpr bool emits_scalar_chaining_execution_proof = false;
        static constexpr bool emits_simd_chaining_execution_proof = false;
        static constexpr bool emits_partial_vulkan_execution_proof = true;
        [[nodiscard, gnu::always_inline]] inline static bool call(
            std::span<const Anchor> anchors,
            const ChainingParams&   params,
            ArenaSlab<Chain>&       chains_out,
            ArenaSlab<Anchor>&      reordered_anchors_out)
        {
            return ::bioseq::chaining::chain_anchors_vulkan(
                anchors, params, chains_out, reordered_anchors_out);
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::chaining_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::VULKAN_TILED_DP);
        }
        static void populate_runtime_params(
            const ChainingParams& params,
            std::vector<NamedProperty>& props) noexcept
        {
            props.push_back({"phase", "A"});
            props.push_back({"fallback", "none"});
            props.push_back({"uses_max_chain_skip", "false"});
            props.push_back({"vulkan_enabled", params.vulkan_enabled ? "true" : "false"});
        }
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"accelerator", "vulkan"},
                {"score_generation", "g3sa-tiled-dp"},
                {"backtrack", "bioseq-cpu"},
                {"uses_max_chain_skip", "false"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// ---------------------------------------------------------------------------
// DOC0263 — Scanner-aware minimizer dispatch.
// For a plain compact_sequence_range or a no-fragment lazy wrapper, runs the
// requested in-RAM kernel (scalar / reference-simd / exact-simd) on contiguous storage
// (zero overhead — the no-fragment lazy forwards to its underlying range). For
// a fragmented lazy wrapper, streams fragments through the bit-identical scalar
// kernel (`collect_minimizers_view`) — see DOC0263 §8.4 for why the scalar path
// is used instead of the SIMD lane-replay.
// ---------------------------------------------------------------------------
enum class MinKernelKind { Scalar, Simd, ExactSimd };

template <MinKernelKind Kind, class MinimizerT,
          sequence_alphabet Alphabet, class Collector>
[[gnu::always_inline]] inline TwoStackExecutionProof minimizer_dispatch(
    Collector& c,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> seq,
    std::size_t w,
    std::size_t k)
{
    if constexpr (Kind == MinKernelKind::ExactSimd) {
        if (seq.bp_size() > static_cast<sequence_size_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            throw ExactSimdRuntimeError{
                TwoStackContinuationStatus::arithmetic_overflow,
                "query minimizer sketch"};
        }
        const auto size = static_cast<std::size_t>(seq.bp_size());
        auto run_exact = [&c, w, k, size](auto resident_view) {
            return collect_minimizers_two_stack_nthash_exact_range_or_throw<
                0, 0, MinimizerT>(
                    c, resident_view, 0u, size, w, k, 0u,
                    "query minimizer sketch");
        };
        const auto access = bioseq::storage::plan_sequence_access(
            seq,
            bioseq::storage::sequence_access_requirement::contiguous_resident,
            0u,
            seq.bp_size(),
            4u);
        if (access.route
            == bioseq::storage::sequence_access_route::borrow_resident) {
            return run_exact(seq);
        }
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(access.admission_bytes);
        if (!permit) {
            throw ExactSimdRuntimeError{
                TwoStackContinuationStatus::non_contiguous_range,
                "query minimizer sketch admission"};
        }
        auto owner = bioseq::storage::materialize_packed_region(
            seq, 0u, seq.bp_size(), 4u, std::move(*permit));
        return run_exact(owner.view());
    } else {
        collect_minimizers_segment_scalar<MinimizerT>(
            c, seq, 0u, seq.bp_size(), w, k);
    }
    return {};
}

template <MinKernelKind Kind, std::size_t W, std::size_t K,
          class MinimizerT, sequence_alphabet Alphabet, class Collector>
[[gnu::always_inline]] inline TwoStackExecutionProof
minimizer_dispatch_constexpr(
    Collector& c,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> seq,
    std::size_t w,
    std::size_t k)
{
    if constexpr (Kind == MinKernelKind::ExactSimd) {
        if (seq.bp_size() > static_cast<sequence_size_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            throw ExactSimdRuntimeError{
                TwoStackContinuationStatus::arithmetic_overflow,
                "constexpr query minimizer sketch"};
        }
        const auto size = static_cast<std::size_t>(seq.bp_size());
        auto run_exact = [&c, w, k, size](auto resident_view) {
            return collect_minimizers_two_stack_nthash_exact_range_or_throw<
                W, K, MinimizerT>(
                    c, resident_view, 0u, size, w, k, 0u,
                    "constexpr query minimizer sketch");
        };
        const auto access = bioseq::storage::plan_sequence_access(
            seq,
            bioseq::storage::sequence_access_requirement::contiguous_resident,
            0u,
            seq.bp_size(),
            4u);
        if (access.route
            == bioseq::storage::sequence_access_route::borrow_resident) {
            return run_exact(seq);
        }
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(access.admission_bytes);
        if (!permit) {
            throw ExactSimdRuntimeError{
                TwoStackContinuationStatus::non_contiguous_range,
                "constexpr query minimizer sketch admission"};
        }
        auto owner = bioseq::storage::materialize_packed_region(
            seq, 0u, seq.bp_size(), 4u, std::move(*permit));
        return run_exact(owner.view());
    } else {
        collect_minimizers_segment_scalar<MinimizerT>(
            c, seq, 0u, seq.bp_size(), w, k);
    }
    return {};
}

// DOC0276 — SUB-RANGE minimizer collection over storage units `[lo_unit, hi_unit)`.
// Used by `minimap_port_segmented` to sketch one target SEGMENT at a time,
// forwarding explicit begin/end iterators (advanced to the unit-aligned segment
// bounds) so positions come out LOCAL to `lo_unit*lpu`.
//
// IMPORTANT: this ALWAYS uses the bit-identical SCALAR `collect_minimizers_view`,
// even for contiguous (no-fragment) sequences. The SIMD two-stack kernel derives
// its iteration length from the WHOLE sequence (`calculate_iteration_offsets` over
// `seq.bp_size()`), not from `(end - begin)`, so handing it a sub-range start/end
// makes it read past the requested window — garbage minimizers + an out-of-bounds
// crash (observed: a 206 Mbp segment of a 500 Mbp target reported 99.7M minimizers
// — more than the WHOLE sequence — then SIGSEGV). The scalar view honours
// `seq_begin/seq_end` exactly and emits `seq_pos` from 0 at `seq_begin`, which is
// what the globalisation (`TargetIndex::add_position_offset`) and the scalar/SIMD
// parity both rely on. Scalar collection over contiguous memory is cheap (no I/O;
// alignment dominates the per-segment wall-clock), so this is correct AND fast.
// DOC0345 Patch D — no default backend. The scalar sub-range body now lives in
// `collect_minimizers_segment_scalar` (bioseq.sequences.kmers) so pipe and the
// persisted builders share one (bug-carrying) sub-range contract. Under
// `VulkanSegment` the GPU segment sketch is called and throws on any decline.
struct MinimizerRangeExecutionProof {
    ScalarMinimizerExecutionProof scalar{};
    bool scalar_present = false;
    TwoStackExecutionProof exact_simd{};
    bool exact_simd_present = false;
    PackedPaperNthashExecutionProof paper_nthash{};
    bool paper_nthash_present = false;
};

/// ExactSimd has two independently valid, but biologically incompatible,
/// minimizer-selection routes for the W=10/K=15 dna4/u32 shape:
///
/// * the resident vector_stream route restored from the report-76 fast path;
/// * the chronological continuation/B8 route used by persisted and fragmented
///   ExactSimd index producers.
///
/// A query cannot select between them from its own storage shape.  Indexed
/// consumers must therefore require the continuation route explicitly; only a
/// caller that owns both full-resident sides of one transient/no-index
/// operation may opt into the resident route.
enum class ExactSimdRangeRoute : std::uint8_t {
    require_continuation,
    require_resident_vector_stream,
};

[[nodiscard]] inline bool exact_simd_proof_matches_range_route(
    ExactSimdRangeRoute route,
    const TwoStackExecutionProof& proof) noexcept
{
    if (route == ExactSimdRangeRoute::require_resident_vector_stream) {
        return proof.resident_vector_stream_kernel_calls > 0u;
    }
    // Check every resident-only field, rather than only the call count, so a
    // partially-populated or mixed proof also fails closed.
    return proof.resident_vector_stream_kernel_calls == 0u
        && proof.resident_vector_stream_single_calls == 0u
        && proof.resident_vector_stream_dual_calls == 0u
        && proof.resident_vector_stream_nonempty_work_calls == 0u
        && proof.resident_vector_stream_input_bp == 0u
        && proof.resident_vector_stream_minimizers_emitted == 0u;
}

/// Compare the biological selection sub-route actually attested by two
/// ExactSimd operations. This is required even when both operations carry the
/// same high-level ExactSimd backend label: resident vector_stream and
/// continuation/B8 proofs are not lookup-compatible.
[[nodiscard]] inline bool exact_simd_proofs_share_selection_route(
    const TwoStackExecutionProof& lhs,
    const TwoStackExecutionProof& rhs) noexcept
{
    if (!lhs.accepted(/*require_nonempty_vector_work=*/false)
        || !rhs.accepted(/*require_nonempty_vector_work=*/false)) {
        return false;
    }
    const bool lhs_resident =
        lhs.resident_vector_stream_kernel_calls > 0u;
    const bool rhs_resident =
        rhs.resident_vector_stream_kernel_calls > 0u;
    return lhs_resident == rhs_resident;
}

/// Validate the complete range-proof payload against the selected CPU sketch
/// backend.  Presence bits are not sufficient evidence on their own: a
/// non-default proof object with a cleared bit is rejected as hidden
/// cross-route evidence.  Vulkan is deliberately not accepted here because
/// this proof type carries only CPU/SIMD operation proofs.
[[nodiscard]] inline bool minimizer_range_proof_matches_backend(
    MinimizerSketchBackend backend,
    const MinimizerRangeExecutionProof& proof,
    bool require_nonempty_vector_work = false,
    ExactSimdRangeRoute exact_simd_route =
        ExactSimdRangeRoute::require_continuation) noexcept
{
    const bool scalar_absent = !proof.scalar_present
        && proof.scalar == ScalarMinimizerExecutionProof{};
    const bool exact_absent = !proof.exact_simd_present
        && proof.exact_simd == TwoStackExecutionProof{};
    const bool paper_absent = !proof.paper_nthash_present
        && proof.paper_nthash == PackedPaperNthashExecutionProof{};

    switch (backend) {
        case MinimizerSketchBackend::ExactSimd:
            return scalar_absent
                && paper_absent
                && proof.exact_simd_present
                && proof.exact_simd.accepted(require_nonempty_vector_work)
                && exact_simd_proof_matches_range_route(
                    exact_simd_route, proof.exact_simd);
        case MinimizerSketchBackend::PaperNtHashSimd:
            return scalar_absent
                && exact_absent
                && proof.paper_nthash_present
                && proof.paper_nthash.accepted(require_nonempty_vector_work);
        case MinimizerSketchBackend::Scalar:
            return proof.scalar_present
                && proof.scalar.accepted(require_nonempty_vector_work)
                && exact_absent
                && paper_absent;
        case MinimizerSketchBackend::VulkanSegment:
            return false;
    }
    return false;
}

template <class MinimizerT, sequence_alphabet Alphabet, class Collector>
[[gnu::always_inline]] inline MinimizerRangeExecutionProof
minimizer_dispatch_range(
    Collector& collector,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> seq,
    sequence_size_t lo_bp,
    sequence_size_t hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    MinimizerSketchBackend backend,
    ExactSimdRangeRoute exact_simd_route =
        ExactSimdRangeRoute::require_continuation)
{
    if (lo_bp > hi_bp || hi_bp > seq.bp_size()) {
        throw std::invalid_argument(
            "minimizer range has an invalid biological-coordinate shape");
    }
    if (backend == MinimizerSketchBackend::VulkanSegment) {
        throw std::invalid_argument(
            "Vulkan minimizer range requires an admitted GPU upload owner");
    }
    if (backend == MinimizerSketchBackend::PaperNtHashSimd) {
        if (window_size != 10u || kmer_size != 15u) {
            throw PaperNtHashRuntimeError(
                "PaperNtHashSimd currently requires W=10 and K=15; "
                "fallback is forbidden");
        }
        if constexpr (!std::same_as<Alphabet, ::seq::dna4>) {
            throw PaperNtHashRuntimeError(
                "PaperNtHashSimd requires the DNA4 alphabet; fallback is forbidden");
        } else {
            auto run_paper = [&collector](const auto& descriptor) {
                return collect_minimizers_packed_paper_nthash_resident_descriptor_checked<
                    15u, 10u, MinimizerT, Alphabet>(collector, descriptor);
            };
            PackedPaperNthashResult result{};
            const auto access = bioseq::storage::plan_sequence_access(
                seq,
                bioseq::storage::sequence_access_requirement::
                    contiguous_resident,
                lo_bp,
                hi_bp,
                sizeof(std::uint32_t));
            if (access.route
                    == bioseq::storage::sequence_access_route::borrow_resident
                && access.resident.has_value()) {
                result = run_paper(*access.resident);
            } else {
                bioseq::parallelism::MemoryAdmissionFailure failure{};
                auto permit =
                    bioseq::parallelism::MemoryAdmissionController::instance()
                        .try_reserve(access.admission_bytes, &failure);
                if (!permit) {
                    throw PaperNtHashRuntimeError(fmt::format(
                        "packed-range admission failed: reason={} "
                        "requested_bytes={} reserved_bytes={} budget_bytes={}",
                        bioseq::parallelism::memory_admission_failure_reason_name(
                            failure.reason),
                        failure.requested_bytes,
                        failure.reserved_bytes,
                        failure.budget_ceiling_bytes));
                }
                auto owner = bioseq::storage::materialize_packed_region(
                    seq, lo_bp, hi_bp, sizeof(std::uint32_t),
                    std::move(*permit));
                result = run_paper(owner.descriptor());
            }
            if (!result.ok()
                || !result.proof.accepted(
                    /*require_nonempty_vector_work=*/false)) {
                throw PaperNtHashRuntimeError(fmt::format(
                    "checked resident kernel declined without fallback: "
                    "status={} lo_bp={} hi_bp={} w={} k={}",
                    static_cast<unsigned>(result.status),
                    lo_bp, hi_bp, window_size, kmer_size));
            }
            return MinimizerRangeExecutionProof{
                .paper_nthash = result.proof,
                .paper_nthash_present = true,
            };
        }
    }
    if (backend == MinimizerSketchBackend::ExactSimd) {
        if (exact_simd_route
                == ExactSimdRangeRoute::require_resident_vector_stream) {
            if constexpr (!std::same_as<Alphabet, ::seq::dna4>
                          || !exact_simd_minimizer_representation<MinimizerT>) {
                throw std::invalid_argument(
                    "resident ExactSimd vector_stream requires DNA4 and an "
                    "exact SIMD minimizer representation");
            } else {
                if (lo_bp != 0u || hi_bp != seq.bp_size()
                    || window_size != 10u || kmer_size != 15u) {
                    throw std::invalid_argument(
                        "resident ExactSimd vector_stream requires the whole "
                        "W=10/K=15 range");
                }
                auto run_resident = [&collector, window_size, kmer_size](
                        const auto& descriptor) {
                    return collect_minimizers_two_stack_nthash_resident_descriptor_or_throw<
                        MinimizerT, Alphabet>(
                            collector, descriptor, window_size, kmer_size);
                };
                TwoStackExecutionProof proof{};
                const auto access = bioseq::storage::plan_sequence_access(
                    seq,
                    bioseq::storage::sequence_access_requirement::
                        contiguous_resident,
                    lo_bp,
                    hi_bp,
                    sizeof(std::uint32_t));
                if (access.route
                        == bioseq::storage::sequence_access_route::
                            borrow_resident
                    && access.resident.has_value()) {
                    proof = run_resident(*access.resident);
                } else {
                    auto permit =
                        bioseq::parallelism::MemoryAdmissionController::instance()
                            .try_reserve(access.admission_bytes);
                    if (!permit) {
                        throw ExactSimdRuntimeError{
                            TwoStackContinuationStatus::non_contiguous_range,
                            "resident vector_stream admission"};
                    }
                    auto owner = bioseq::storage::materialize_packed_region(
                        seq, lo_bp, hi_bp, sizeof(std::uint32_t),
                        std::move(*permit));
                    proof = run_resident(owner.descriptor());
                }
                return MinimizerRangeExecutionProof{
                    .exact_simd = proof,
                    .exact_simd_present = true,
                };
            }
        }
        const sequence_size_t length = hi_bp - lo_bp;
        if (length > static_cast<sequence_size_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            throw ExactSimdRuntimeError{
                TwoStackContinuationStatus::arithmetic_overflow,
                "target minimizer range sketch"};
        }
        auto run_exact = [&collector, window_size, kmer_size](
                auto resident_view,
                std::size_t begin,
                std::size_t end) {
            return collect_minimizers_two_stack_nthash_exact_range_or_throw<
                0, 0, MinimizerT>(
                    collector, resident_view, begin, end,
                    window_size, kmer_size, 0u,
                    "target minimizer range sketch");
        };
        TwoStackExecutionProof proof{};
        const auto access = bioseq::storage::plan_sequence_access(
            seq,
            bioseq::storage::sequence_access_requirement::contiguous_resident,
            lo_bp,
            hi_bp,
            4u);
        if (access.route
            == bioseq::storage::sequence_access_route::borrow_resident) {
            proof = run_exact(
                seq,
                static_cast<std::size_t>(lo_bp),
                static_cast<std::size_t>(hi_bp));
        } else {
            auto permit =
                bioseq::parallelism::MemoryAdmissionController::instance()
                    .try_reserve(access.admission_bytes);
            if (!permit) {
                throw ExactSimdRuntimeError{
                    TwoStackContinuationStatus::non_contiguous_range,
                    "target minimizer range sketch admission"};
            }
            auto owner = bioseq::storage::materialize_packed_region(
                seq, lo_bp, hi_bp, 4u, std::move(*permit));
            proof = run_exact(
                owner.view(), 0u, static_cast<std::size_t>(length));
        }
        return MinimizerRangeExecutionProof{
            .exact_simd = proof,
            .exact_simd_present = true,
        };
    }
    if (backend != MinimizerSketchBackend::Scalar) {
        throw std::invalid_argument(
            "minimizer range received an unsupported backend");
    }
    collect_minimizers_segment_scalar<MinimizerT>(
        collector, seq, lo_bp, hi_bp, window_size, kmer_size);
    ScalarMinimizerExecutionProof proof{};
    proof.record(hi_bp - lo_bp);
    return MinimizerRangeExecutionProof{
        .scalar = proof,
        .scalar_present = true,
    };
}

/// Dispatch one complete indexed-query sketch through the canonical facade.
template <class MinimizerT, sequence_alphabet Alphabet, class Collector>
[[gnu::always_inline]] inline MinimizerRangeExecutionProof
minimizer_dispatch_indexed_query_whole(
    Collector& collector,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_sequence,
    std::size_t window_size,
    std::size_t kmer_size,
    MinimizerSketchBackend backend) {
    return minimizer_dispatch_range<MinimizerT>(
        collector,
        query_sequence,
        0u,
        query_sequence.bp_size(),
        window_size,
        kmer_size,
        backend,
        ExactSimdRangeRoute::require_continuation);
}

// Resolve the biological target-sketch backend from the minimizer selector
// itself. Chaining and alignment choices must not silently substitute Scalar
// sketch semantics for an explicitly selected ExactSimd/Paper producer.
// Unsupported SIMD shapes fail in their checked drivers.
[[nodiscard]] inline MinimizerSketchBackend
target_index_sketch_backend_from_params(const Minimap2Params& params) noexcept
{
    if (params.minimizer_algorithm
        == MinimizerAlgorithmType::PAPER_NTHASH_SIMD) {
        return MinimizerSketchBackend::PaperNtHashSimd;
    }
    if (params.vulkan_enabled
        && params.vulkan_gpu_index_build_enabled
        && params.minimizer_algorithm
            == MinimizerAlgorithmType::VULKAN_MINIMAP_INT) {
        return MinimizerSketchBackend::VulkanSegment;
    }
    if (params.minimizer_algorithm
        == MinimizerAlgorithmType::TWO_STACK_NTHASH) {
        return MinimizerSketchBackend::ExactSimd;
    }
    return MinimizerSketchBackend::Scalar;
}

// DOC0276 — segment GEOMETRY. The MinimapSketchData position field is 28 bits
// (MinimizerRepr<28,15>), so a segment's COLLECTED span — core + an overlap on
// each side — must stay within that field, or the global position handed back by
// `add_position_offset` would be silently masked by the packing ctor
// (`pos_v & pos_field_mask`, index.cppm) and corrupt the seed. We cap the core at
// 200 Mbp and each overlap at 30 Mbp; the static_assert ties both to the REAL
// field width so neither can be bumped past 2^28 without a compile error.
inline constexpr sequence_size_t kSegmentCoreMaxBp    = 200'000'000ull;  // core ceiling
inline constexpr sequence_size_t kSegmentOverlapMaxBp =  30'000'000ull;  // per-side overlap ceiling
inline constexpr sequence_size_t kSegmentMinBp        =  32'000'000ull;  // floor (keep batches useful)
inline constexpr sequence_size_t kVulkanSegmentMinBp   =   1'000'000ull;  // floor when GPU sketch is selected
// DOC0353 continuation: 76M/96M is the best measured consolidation point. The
// earlier 100M/128M experiment made the inner sketch admission shrink and exposed
// a multi-window correctness bug; 96M keeps the collected span below that failed
// shape while cutting the 300M probe from ~44 GPU windows to ~6 when
// memory admission accepts a one-window sketch.
inline constexpr sequence_size_t kVulkanSegmentTargetBp = 76'000'000ull;  // target core before query-overlap clamping
inline constexpr sequence_size_t kVulkanSegmentCollectedMaxBp =
    96'000'000ull;
static_assert(kSegmentCoreMaxBp + 2 * kSegmentOverlapMaxBp <=
                  minimizer_max_sequence_length_v<MinimapSketchData>,
              "segment collected span (core + 2*overlap) must fit "
              "MinimapSketchData's 28-bit position field");

// Choose a target-index SEGMENT size (bp) from the live memory budget. A full
// minimizer index over an N-bp target costs ~6 B/bp at PEAK (collector vector
// 8 B + flat_map bucket ~16 B/distinct + positions 8 B, all × density ~0.19).
// On a 16 GB box a 6 Gbp full index (~37 GB) swaps; capping a segment to the
// budget keeps the live index ~one segment, clamped to [kSegmentMinBp,
// kSegmentCoreMaxBp].
[[nodiscard]] inline sequence_size_t segment_bp_from_budget() noexcept {
    // Respect both the live process/system ceiling and the explicit index cap.
    // The old raw default-budget read ignored
    // MemoryAdmissionController::set_index_memory_cap_bytes(), so admission
    // and segment geometry could disagree about the maximum legal index.
    const std::uint64_t budget =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .budget_ceiling();
    // allocatable_bytes() == UINT64_MAX when the mach/POSIX probe is unavailable;
    // dividing keeps it huge so the clamp yields kSegmentCoreMaxBp (the default).
    const std::uint64_t by_budget =
        budget / bioseq::parallelism::kMinimizerIndexPeakBytesPerBp;
    const sequence_size_t want =
        (by_budget > kSegmentCoreMaxBp) ? kSegmentCoreMaxBp
                                        : static_cast<sequence_size_t>(by_budget);
    return want < kSegmentMinBp ? kSegmentMinBp : want;
}

[[nodiscard]] inline sequence_size_t segment_bp_from_budget_divided(
        unsigned divisor,
        sequence_size_t floor_bp = kSegmentMinBp) noexcept {
    const sequence_size_t base = segment_bp_from_budget();
    const sequence_size_t divided = divisor <= 1 ? base : base / divisor;
    return divided < floor_bp ? floor_bp : divided;
}

[[nodiscard]] inline sequence_size_t segment_overlap_bp(
        sequence_size_t query_len) noexcept {
    sequence_size_t overlap_bp = query_len + query_len / 4 + 100'000;
    if (overlap_bp > kSegmentOverlapMaxBp) {
        overlap_bp = kSegmentOverlapMaxBp;
    }
    return overlap_bp;
}

[[nodiscard]] inline sequence_size_t vulkan_segment_target_bp(
        sequence_size_t query_len) noexcept {
    const sequence_size_t overlap_bp = segment_overlap_bp(query_len);
    const sequence_size_t expanded_overlap =
        overlap_bp > kVulkanSegmentCollectedMaxBp / 2
            ? kVulkanSegmentCollectedMaxBp
            : overlap_bp * 2;
    if (expanded_overlap + kVulkanSegmentMinBp
        >= kVulkanSegmentCollectedMaxBp) {
        return kVulkanSegmentMinBp;
    }
    const sequence_size_t room =
        kVulkanSegmentCollectedMaxBp - expanded_overlap;
    const sequence_size_t target = std::min(room, kVulkanSegmentTargetBp);
    return target < kVulkanSegmentMinBp ? kVulkanSegmentMinBp : target;
}

[[nodiscard]] inline unsigned
segment_geometry_divisor_for_backend(
        MinimizerSketchBackend backend,
        sequence_size_t query_len = 0) noexcept {
    if (backend != MinimizerSketchBackend::VulkanSegment) {
        return 1u;
    }
    const sequence_size_t base = segment_bp_from_budget();
    const sequence_size_t target = vulkan_segment_target_bp(query_len);
    const sequence_size_t divisor =
        (base + target - sequence_size_t{1}) / target;
    return static_cast<unsigned>(std::max<sequence_size_t>(1, divisor));
}

[[nodiscard]] inline sequence_size_t
segment_core_floor_for_backend(MinimizerSketchBackend backend) noexcept {
    return backend == MinimizerSketchBackend::VulkanSegment
        ? kVulkanSegmentMinBp
        : kSegmentMinBp;
}

// DOC0279 §11.3a — the segment geometry of `minimap_port_segmented`, extracted
// so `dispatch_buffer`'s stored-index assemble loop reuses the EXACT recompute
// geometry. NOT a pure function of its args: it reads the runtime budget via
// `segment_bp_from_budget()` exactly as recompute does. Each entry covers a
// segment's overlapped collection range `[cs_u, ce_u)` (in units) and its core
// claim band `[core_lo_bp, core_hi_bp)`.
[[nodiscard]] inline std::vector<::bioseq::indexing::SegmentUnitRange> segment_unit_ranges(
        sequence_size_t total_bp,
        sequence_size_t query_len,
        std::size_t lpu,
        unsigned divisor,
        sequence_size_t floor_bp) {
    std::vector<::bioseq::indexing::SegmentUnitRange> out;
    if (total_bp == 0 || lpu == 0) return out;
    const sequence_size_t total_units = (total_bp + lpu - 1) / lpu;
    const sequence_size_t overlap_bp = segment_overlap_bp(query_len);
    const sequence_size_t ovl_units = (overlap_bp + lpu - 1) / lpu;
    sequence_size_t seg_units =
        segment_bp_from_budget_divided(divisor, floor_bp) / lpu;
    if (seg_units == 0) seg_units = 1;
    for (sequence_size_t lo_u = 0; lo_u < total_units; lo_u += seg_units) {
        const sequence_size_t hi_u = std::min(lo_u + seg_units, total_units);
        const sequence_size_t cs_u = (lo_u > ovl_units) ? (lo_u - ovl_units) : 0;
        const sequence_size_t ce_u = std::min(hi_u + ovl_units, total_units);
        const sequence_size_t core_lo_bp = lo_u * lpu;
        const sequence_size_t core_hi_bp = std::min(hi_u * lpu, total_bp);
        out.push_back(::bioseq::indexing::SegmentUnitRange{
            cs_u, ce_u, core_lo_bp, core_hi_bp});
    }
    return out;
}

[[nodiscard]] inline std::vector<::bioseq::indexing::SegmentUnitRange> segment_unit_ranges(
        sequence_size_t total_bp, sequence_size_t query_len, std::size_t lpu, unsigned divisor) {
    return segment_unit_ranges(total_bp, query_len, lpu, divisor, kSegmentMinBp);
}

[[nodiscard]] inline std::vector<::bioseq::indexing::SegmentUnitRange> segment_unit_ranges(
        sequence_size_t total_bp, sequence_size_t query_len, std::size_t lpu) {
    return segment_unit_ranges(total_bp, query_len, lpu, 1);
}

[[nodiscard]] inline std::vector<::bioseq::indexing::SegmentUnitRange> segment_unit_ranges_for_backend(
        sequence_size_t total_bp,
        sequence_size_t query_len,
        std::size_t lpu,
        MinimizerSketchBackend backend) {
    return segment_unit_ranges(
        total_bp,
        query_len,
        lpu,
        segment_geometry_divisor_for_backend(backend, query_len),
        segment_core_floor_for_backend(backend));
}

[[nodiscard]] inline std::pair<sequence_size_t, sequence_size_t>
validated_segment_bp_bounds(
        const ::bioseq::indexing::SegmentUnitRange& range,
        std::size_t letters_per_unit,
        sequence_size_t total_bp) {
    const sequence_size_t lpu =
        static_cast<sequence_size_t>(letters_per_unit);
    if (letters_per_unit == 0u
        || static_cast<std::size_t>(lpu) != letters_per_unit) {
        throw std::invalid_argument(
            "segment letters-per-unit is not representable");
    }
    constexpr sequence_size_t max_bp =
        (std::numeric_limits<sequence_size_t>::max)();
    if (range.cs_u > max_bp / lpu || range.ce_u > max_bp / lpu) {
        throw std::overflow_error(
            "segment packed-unit bounds overflow biological coordinates");
    }
    const sequence_size_t lo_bp = range.cs_u * lpu;
    const sequence_size_t hi_bp =
        std::min<sequence_size_t>(range.ce_u * lpu, total_bp);
    if (lo_bp > hi_bp || lo_bp > total_bp) {
        throw std::invalid_argument(
            "segment packed-unit bounds exceed the biological sequence");
    }
    return {lo_bp, hi_bp};
}

// ---------------------------------------------------------------------------
// 8.0.2 MinimizerAlgorithmType kernel traits
// ---------------------------------------------------------------------------
//
// The minimizer kernels are templated on the sketch-collector and the
// sequence range, both of which depend on the MinimizerStage's `MinimizerT`
// and `QR` template parameters.  The trait's `call` method is therefore a
// member template — the stage instantiates it with the correct types at the
// call site.

/// Scalar minimizer extraction.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR> {
    struct kernel_type {
        static constexpr bool emits_scalar_minimizer_execution_proof = true;
        static constexpr bool emits_exact_simd_execution_proof = false;
        static constexpr bool emits_paper_nthash_execution_proof = false;
        static constexpr bool emits_partial_vulkan_execution_proof = false;
        template <class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static ScalarMinimizerExecutionProof call(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            minimizer_dispatch<MinKernelKind::Scalar, MinimizerT>(
                collector, range, window_size, kmer_size);
            ScalarMinimizerExecutionProof proof{};
            proof.record(static_cast<std::uint64_t>(range.bp_size()));
            return proof;
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static ScalarMinimizerExecutionProof call_constexpr(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range)
        {
            minimizer_dispatch_constexpr<MinKernelKind::Scalar, W, K, MinimizerT>(
                collector, range, W, K);
            ScalarMinimizerExecutionProof proof{};
            proof.record(static_cast<std::uint64_t>(range.bp_size()));
            return proof;
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "false"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Reference SIMD minimizer extraction (single-stack).
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW> {
    struct kernel_type {
        static constexpr bool emits_scalar_minimizer_execution_proof = false;
        static constexpr bool emits_exact_simd_execution_proof = false;
        static constexpr bool emits_paper_nthash_execution_proof = false;
        static constexpr bool emits_partial_vulkan_execution_proof = false;
        template <class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            minimizer_dispatch<MinKernelKind::Simd, MinimizerT>(
                collector, range, window_size, kmer_size);
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call_constexpr(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range)
        {
            minimizer_dispatch_constexpr<MinKernelKind::Simd, W, K, MinimizerT>(
                collector, range, W, K);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "true"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Two-stack nthash minimizer extraction (current default).
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::TWO_STACK_NTHASH> {
    struct kernel_type {
        static constexpr bool emits_scalar_minimizer_execution_proof = false;
        static constexpr bool emits_exact_simd_execution_proof = true;
        static constexpr bool emits_paper_nthash_execution_proof = false;
        static constexpr bool emits_partial_vulkan_execution_proof = false;
        template <class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static TwoStackExecutionProof call(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            return minimizer_dispatch<MinKernelKind::ExactSimd, MinimizerT>(
                collector, range, window_size, kmer_size);
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static TwoStackExecutionProof call_constexpr(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range)
        {
            return minimizer_dispatch_constexpr<MinKernelKind::ExactSimd, W, K, MinimizerT>(
                collector, range, W, K);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::TWO_STACK_NTHASH);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "true"},
                {"two_stack",  "true"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Bounded-memory paper-u32/R7/sum ntHash SIMD minimizer extraction. This is
/// deliberately distinct from TWO_STACK_NTHASH/ExactSimd: both the execution
/// proof and persisted scheme identity must match this selector.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::PAPER_NTHASH_SIMD> {
    struct kernel_type {
        static constexpr bool emits_scalar_minimizer_execution_proof = false;
        static constexpr bool emits_exact_simd_execution_proof = false;
        static constexpr bool emits_paper_nthash_execution_proof = true;
        static constexpr bool emits_partial_vulkan_execution_proof = false;

        template <class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static PackedPaperNthashExecutionProof call(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            const MinimizerRangeExecutionProof proof =
                minimizer_dispatch_range<MinimizerT>(
                    collector,
                    range,
                    0,
                    static_cast<sequence_size_t>(range.size()),
                    window_size,
                    kmer_size,
                    MinimizerSketchBackend::PaperNtHashSimd);
            if (!proof.paper_nthash_present) {
                throw PaperNtHashRuntimeError(
                    "PaperNtHashSimd leaf returned without Paper execution proof");
            }
            return proof.paper_nthash;
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static PackedPaperNthashExecutionProof call_constexpr(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
            Collector& collector, Range& range)
        {
            return call<MinimizerT>(params, collector, range, W, K);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::PAPER_NTHASH_SIMD);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "true"},
                {"logical_streams", "8"},
                {"hash", "paper-u32-nthash-r7-sum"},
                {"fallback", "forbidden"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// DOC0351 Phase C — Vulkan minimap-int minimizer extraction. Reads packed
/// `compact_sequence_range` storage directly on the GPU. When this algorithm
/// is selected, Vulkan execution is mandatory and failures throw.
/// `call_result` exposes the resident device-minimizer buffer so the all-Vulkan
/// sketch+seed path reuses it without a second upload (`may_emit_device_minimizers`).
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::VULKAN_MINIMAP_INT> {
    struct kernel_type {
        static constexpr bool emits_scalar_minimizer_execution_proof = false;
        static constexpr bool emits_exact_simd_execution_proof = false;
        static constexpr bool emits_paper_nthash_execution_proof = false;
        static constexpr bool emits_partial_vulkan_execution_proof = true;
        static constexpr bool may_emit_device_minimizers = true;

        template <class MinimizerT, class Collector, class Range>
        [[nodiscard, gnu::always_inline]] static
        bioseq::gpu::vulkan::VulkanSketchDispatchToken call(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            if (!params.vulkan_enabled) {
                throw_vulkan_declined("Vulkan minimizer sketch");
            }
            bioseq::gpu::vulkan::VulkanSketchDecline decline{};
            auto token =
                bioseq::gpu::vulkan::collect_minimizers_vulkan<MinimizerT>(
                    collector, range, window_size, kmer_size, &decline);
            if (!token.has_value() || !token->accepted()) {
                throw_vulkan_sketch_declined(
                    "Vulkan minimizer sketch", decline);
            }
            return *token;
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[nodiscard, gnu::always_inline]] static
        bioseq::gpu::vulkan::VulkanSketchDispatchToken call_constexpr(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
            Collector& collector, Range& range)
        {
            return call<MinimizerT>(params, collector, range, W, K);
        }

        template <class MinimizerT, class Range>
        [[nodiscard]] static std::optional<
            bioseq::gpu::vulkan::VulkanSketchResult<MinimizerT>>
        call_result(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
            Range& range, std::size_t window_size, std::size_t kmer_size,
            bioseq::gpu::vulkan::VulkanSketchHostOwnershipRoute
                host_ownership_route = bioseq::gpu::vulkan::
                    VulkanSketchHostOwnershipRoute::collector_compatible)
        {
            if (!params.vulkan_enabled) {
                throw_vulkan_declined("Vulkan minimizer sketch");
            }
            bioseq::gpu::vulkan::VulkanSketchDecline decline{};
            auto result = bioseq::gpu::vulkan::collect_minimizers_vulkan_result<MinimizerT>(
                range,
                window_size,
                kmer_size,
                &decline,
                host_ownership_route);
            if (!result) {
                throw_vulkan_sketch_declined(
                    "Vulkan minimizer sketch", decline);
            }
            return result;
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::VULKAN_MINIMAP_INT);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
            std::vector<NamedProperty>& props) noexcept
        {
            props.push_back({"accelerator", "vulkan"});
            props.push_back({"vulkan_enabled", p.vulkan_enabled ? "true" : "false"});
        }
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"hash", "minimap-int"},
                {"alphabet_dispatch", "runtime-shader-key"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// ---------------------------------------------------------------------------
// 8.0.3 BaseAlignmentAlgorithmType kernel traits (DOC0254 §6.2)
// ---------------------------------------------------------------------------
//
// The trait pattern is now universal across every algorithm dimension
// (DOC0254 R10).  AlignStage<Kernel, QR, TR> calls `Kernel::template
// call<QR, TR>(...)` once — the per-algorithm dispatch lives entirely in
// the trait specialisations below.

namespace detail {

/// Shared helper for KSW2 / Block Aligner / KSW2_BATCHED — pushes the
/// four affine-scoring parameters that every base aligner consumes.
[[gnu::always_inline]] inline void push_base_affine_params(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
    std::vector<NamedProperty>& out)
{
    out.push_back({"match_score",  std::to_string(p.match_score)});
    out.push_back({"mismatch_pen", std::to_string(p.mismatch_pen)});
    out.push_back({"gap_open1",    std::to_string(p.gap_open1)});
    out.push_back({"gap_ext1",     std::to_string(p.gap_ext1)});
}

}  // namespace detail

template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2> {
    struct kernel_type {
        /// DOC0254 R10: the dispatcher consults this constant to route the
        /// cross-query deferred-align scheduling shape instead of an
        /// `if constexpr (BAA == ...)` ladder.
        static constexpr bool supports_cross_query_batched = false;
        static constexpr bool emits_block_execution_proof = false;
        static constexpr bool emits_scalar_ksw2_execution_proof = true;
        static constexpr bool emits_ksw2_batched_execution_proof = false;

        template <sequence_alphabet Alphabet>
        [[gnu::always_inline]] static void call(
            std::span<AlignRegion>                       regions,
            std::span<const ::bioseq::chaining::Anchor>  anchors,
            bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
            bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
            const ScoringMatrix<Alphabet>&               scoring_matrix,
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
            BlockAlignerExecutionProof* forbidden_block_proof,
            ScalarPipelineExecutionProof* scalar_execution_proof)
        {
            if (forbidden_block_proof != nullptr) {
                block_aligner_forbidden_ksw2_fallback(*forbidden_block_proof);
            }
            ::bioseq::simd::HWY_NAMESPACE::align_regions<
                ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2>(
                regions, anchors, query_seq, target_seq, scoring_matrix, params,
                /*block_execution_proof=*/nullptr,
                scalar_execution_proof);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return "KSW2";
        }

        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
            std::vector<NamedProperty>& out)
        {
            detail::push_base_affine_params(p, out);
            out.push_back({"gap_open2", std::to_string(p.gap_open2)});
            out.push_back({"gap_ext2",  std::to_string(p.gap_ext2)});
            out.push_back({"zdrop",     std::to_string(p.zdrop)});
            out.push_back({"end_bonus", std::to_string(p.end_bonus)});
        }

        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised",        "intra-sequence striped"},
                {"single_affine_eq",  "data-dependent"},
                {"specialised",       Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::BLOCK_ALIGNER> {
    struct kernel_type {
        static constexpr bool supports_cross_query_batched = false;
        static constexpr bool emits_block_execution_proof = true;
        static constexpr bool emits_scalar_ksw2_execution_proof = false;
        static constexpr bool emits_ksw2_batched_execution_proof = false;

        template <sequence_alphabet Alphabet>
        [[gnu::always_inline]] static void call(
            std::span<AlignRegion>                       regions,
            std::span<const ::bioseq::chaining::Anchor>  anchors,
            bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
            bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
            const ScoringMatrix<Alphabet>&               scoring_matrix,
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
            BlockAlignerExecutionProof* block_execution_proof,
            ScalarPipelineExecutionProof* /*scalar_execution_proof*/)
        {
            ::bioseq::simd::HWY_NAMESPACE::align_regions<
                ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::BLOCK_ALIGNER>(
                regions, anchors, query_seq, target_seq, scoring_matrix, params,
                block_execution_proof);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return "Block Aligner";
        }

        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
            std::vector<NamedProperty>& out)
        {
            detail::push_base_affine_params(p, out);
            out.push_back({"block_min_size",
                std::to_string(static_cast<int32_t>(p.block_min_size))});
            out.push_back({"block_max_size",
                std::to_string(static_cast<int32_t>(p.block_max_size))});
        }

        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised",       "block-affine"},
                {"single_affine_only", "true"},
                {"specialised",      Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// `KSW2_BATCHED` — DOC0254 §11.  The kernel ships in
// `alignment_hwy/ksw2_batched.cppm`; this trait forwards through a free
// function `align_regions_batched<QR, TR>(...)` that is declared in this
// module via the `bioseq_sequences.ksw2_batched` import.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2_BATCHED> {
    struct kernel_type {
        /// DOC0254 §11.10 — the only kernel whose dispatcher branch
        /// supports cross-query batching.  Setting this constant true on
        /// the KSW2_BATCHED specialisation only is how
        /// `pipeline_run_impl_batch` selects the deferred-align variant
        /// without an enum-value `if constexpr` ladder.
        static constexpr bool supports_cross_query_batched = true;
        static constexpr bool emits_block_execution_proof = false;
        static constexpr bool emits_scalar_ksw2_execution_proof = false;
        static constexpr bool emits_ksw2_batched_execution_proof = true;

        template <sequence_alphabet Alphabet>
        [[gnu::always_inline]] static void call(
            std::span<AlignRegion>                       regions,
            std::span<const ::bioseq::chaining::Anchor>  anchors,
            bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
            bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
            const ScoringMatrix<Alphabet>&               scoring_matrix,
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
            BlockAlignerExecutionProof* forbidden_block_proof,
            ScalarPipelineExecutionProof* /*scalar_execution_proof*/,
            Ksw2BatchedExecutionProof* execution_proof = nullptr)
        {
            if (forbidden_block_proof != nullptr) {
                block_aligner_forbidden_ksw2_fallback(*forbidden_block_proof);
            }
            ::bioseq::simd::HWY_NAMESPACE::align_regions_batched<Alphabet>(
                regions, anchors, query_seq, target_seq, scoring_matrix, params,
                execution_proof);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return "KSW2 (inter-region batched)";
        }

        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
            std::vector<NamedProperty>& out)
        {
            // Same runtime knobs as the intra-sequence KSW2 path; the
            // bucketing/batching is determined by the kernel from
            // params.max_gap, params.zdrop, and the per-fill geometry.
            algorithm_traits<
                ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2>::
                kernel_type::populate_runtime_params(p, out);
        }

        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised",        "inter-sequence packed"},
                {"bucket_widths",     "[<=32, <=64, <=128, <=256, >256]"},
                {"long_fill_kernel",  "intra-sequence KSW2 (fallback)"},
                {"specialised",       Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// =============================================================================
// 11. Metadata surface (DOC0249 §11) — full definitions
// =============================================================================
//
// `NamedProperty` was hoisted ahead of the algorithm_traits block so trait
// signatures can return `std::vector<NamedProperty>`.  The remaining
// description types are defined here in their natural source order.

struct StageAlgorithmDescription {
    std::string                name;
    std::vector<NamedProperty> parameters;
    std::vector<NamedProperty> computed_properties;
    std::optional<std::string> extra_description;
};

struct StageDescription {
    std::string                              stage_name;
    std::optional<StageAlgorithmDescription> algorithm;
    std::optional<std::string>               extra_description;

    /**
     * @brief Pre-allocation upper bound the stage reserves before driving its
     * kernel (DOC0249 §21 — "reserve correctness").
     *
     * Populated by every stage that opens an `ArenaSlab<E>` of bounded size
     * (MinimizerStage, SeedHitStage, ChainStage, RegionStage).  Set to
     * `std::nullopt` for stages that mutate in-place or whose output size
     * is `0` (IndexStage, SinkStage, etc.).  The EXPLAIN renderer surfaces
     * this as a per-stage `[reserved: N]` line so a reader can verify
     * `actual ≤ reserved` against the runtime instrumentation log.
     */
    std::optional<std::size_t>               estimated_batch_size;
};

struct PipelineDescription {
    std::vector<StageDescription> stages;
};

// =============================================================================
// 21. Output size estimates (DOC0249 §21)
// =============================================================================

namespace detail {

[[nodiscard]] constexpr std::size_t estimated_sketch_count(
    std::size_t bp_size, int32_t window_size) noexcept {
    if (window_size <= 0) return bp_size + 64u;
    return bp_size / static_cast<std::size_t>(window_size) + 64u;
}

/**
 * @brief Reserve-time anchor estimate (DOC0249 §21.3).
 *
 * Uses today's median heuristic (2× sketch_count) but clamps by the
 * physical shared-k-mer ceiling so very-short query/target pairs don't
 * over-allocate.  The slab grows geometrically when the kernel produces
 * more anchors than estimated.
 */
[[nodiscard]] constexpr std::size_t estimated_anchor_count(
    std::size_t sketch_count, std::size_t query_bp, std::size_t target_bp,
    int32_t kmer_size) noexcept
{
    const std::size_t shared_ceiling =
        std::min(query_bp, target_bp) > static_cast<std::size_t>(kmer_size)
            ? std::min(query_bp, target_bp) - static_cast<std::size_t>(kmer_size) + 1u
            : sketch_count;
    const std::size_t median_estimate = sketch_count * 2u;
    return std::min(median_estimate, shared_ceiling);
}

[[nodiscard]] constexpr std::size_t estimated_chain_count(
    std::size_t anchor_count, int32_t min_seed_count) noexcept {
    if (min_seed_count <= 0) return anchor_count;
    return anchor_count / static_cast<std::size_t>(min_seed_count) + 1u;
}

[[nodiscard]] constexpr std::size_t estimated_region_count(
    std::size_t chain_count, int32_t best_n) noexcept {
    return std::min(chain_count,
                    static_cast<std::size_t>(best_n) + 16u);
}

/**
 * @brief Upper-bound estimate of the per-region CIGAR op count.
 *
 * DOC0254 §10.3 unifies the helper signature so it can model the post-fill
 * outer extents instead of `region.block_length` (which excludes the
 * left/right kernel extensions).  Describe-time callers pass
 * `(block_length, block_length)` as a coarse approximation — the value is
 * only a render hint, never an actual reservation.
 */
[[nodiscard]] constexpr std::size_t estimated_cigar_op_count(
    int32_t query_extent, int32_t ref_extent) noexcept {
    const int32_t q = query_extent > 0 ? query_extent : 0;
    const int32_t r = ref_extent   > 0 ? ref_extent   : 0;
    return static_cast<std::size_t>(q) + static_cast<std::size_t>(r) + 16u;
}

[[nodiscard]] constexpr std::size_t estimated_result_count(
    sequence_size_t query_len, sequence_size_t target_len,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    const std::size_t bp_floor = std::max<std::size_t>(1u,
        static_cast<std::size_t>(std::min(query_len, target_len)) / 500u);
    return std::max(static_cast<std::size_t>(params.best_n + 4), bp_floor);
}

} // namespace detail

// =============================================================================
// 9.6 Derived helpers (DOC0249 §9.6)
// =============================================================================

[[nodiscard]] constexpr int32_t sub_diff(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return params.match_score * 2 + params.mismatch_pen;
}

[[nodiscard]] constexpr int32_t min_diff(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return params.kmer_size * 2;
}

/**
 * @brief Thread-local cached scoring matrix keyed on (match, mismatch, ambi).
 *
 * Mirrors the cache previously inlined at `minimap_port.cppm:2392-2408`
 * inside `minimap2_port_with_index_impl`.  Hoisted to a free function so
 * both the legacy entry point and `pipeline_run_impl_batch` can share it.
 */
template <sequence_alphabet Alphabet>
[[nodiscard, gnu::always_inline]] inline const ScoringMatrix<Alphabet>&
thread_local_scoring_matrix(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    thread_local int8_t cached_match    = std::numeric_limits<int8_t>::min();
    thread_local int8_t cached_mismatch = std::numeric_limits<int8_t>::min();
    thread_local int8_t cached_ambi     = std::numeric_limits<int8_t>::min();
    thread_local ScoringMatrix<Alphabet> cached_matrix{};

    const int8_t want_match    = static_cast<int8_t>(params.match_score);
    const int8_t want_mismatch = static_cast<int8_t>(params.mismatch_pen);
    const int8_t want_ambi     = static_cast<int8_t>(params.sc_ambi);
    if (cached_match != want_match
        || cached_mismatch != want_mismatch
        || cached_ambi != want_ambi)
    {
        cached_matrix   = ScoringMatrix<Alphabet>::generate(
            want_match, want_mismatch, want_ambi);
        cached_match    = want_match;
        cached_mismatch = want_mismatch;
        cached_ambi     = want_ambi;
    }
    return cached_matrix;
}

// =============================================================================
// 8. Concrete stages (DOC0249 §8)
// =============================================================================
//
// Each stage's `process*<Cep>(...)` body forwards to the existing kernel
// function in `bioseq::simd::HWY_NAMESPACE` (or the chaining/cigar modules)
// without renaming the kernel.  Wire-type between stages is `std::span<E>`
// directly; arena ownership is provided by `ArenaSlab<E>` materialised
// inside each stage before the downstream call.

/**
 * @brief Stage 0: minimizer extraction.
 *
 * Templated on `Kernel` — the algorithm-specific call surface exposed by
 * `algorithm_traits<MinimizerAlgorithmType::X>::kernel_type` (DOC0249 §8.0,
 * R5 trait pattern).  The stage body has zero `if constexpr` over enum
 * values; adding a new minimizer-extraction algorithm is a single trait
 * specialisation in the kernel's own module.
 *
 * The stage ignores its input batch and instead sketches the pre-captured
 * `query_seq_` pointer (DOC0249 §8.1).
 */
template <class MinimizerT,
          class Kernel,
          sequence_alphabet Alphabet>
struct MinimizerStage
    : StageBase<MinimizerStage<MinimizerT, Kernel, Alphabet>, MinimizerT>
{
    static constexpr std::string_view stage_name = "MinimizerStage";

    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq_;
    int32_t runtime_w;
    int32_t runtime_k;
    // DOC0307 §5.1 — runtime flags so the Vulkan kernel can gate on
    // `vulkan_enabled` without touching Runtime when Vulkan is off. CPU kernels
    // ignore it.
    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    MatchResultCollection* proof_dst = nullptr;
    MatchResultCollection* host_timing_dst = nullptr;

    // DOC0254 §5.4 / §8.4: the kernel writes directly into the per-query
    // arena slab via `sketch_collector_arena_adapter`, eliminating the
    // sketches→vector→slab memcpy that the previous shape required.  The
    // specialised path calls `call_constexpr<W, K, MinimizerT>(...)` so
    // the (W, K) pair propagates as integer immediates through the
    // window-loop bounds and unrolls.
    template <is_constexpr_aligner_params Cep, class B, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process(B&& /*ignored*/, EmitToken<Sink>&& token) {
        const std::uint64_t host_stage_started = host_timing_dst == nullptr
            ? 0u : host_stage_now_ns();
        const std::size_t window_size = Cep::generic_fallback
            ? static_cast<std::size_t>(runtime_w)
            : Cep::window_len;
        const std::size_t kmer_size = Cep::generic_fallback
            ? static_cast<std::size_t>(runtime_k)
            : Cep::kmer_len;

        // Validate the zero-based position domain before any scalar, SIMD, or
        // Vulkan kernel can construct a masked MinimizerRepr. A query shorter
        // than k is valid and naturally emits an empty minimizer batch.
        require_representable_minimizer_query_length<MinimizerT>(
            query_seq_.bp_size());

        // DOC0307 §5.1 — device-minimizer path: only the Vulkan kernel exposes
        // `call_result`. When it returns a result, copy the host minimizers into
        // the arena slab and forward the resident device span as stage-local
        // metadata so SeedHitStage can skip a second query-minimizer upload.
        if constexpr (requires { Kernel::may_emit_device_minimizers; }) {
            if constexpr (Kernel::may_emit_device_minimizers) {
                if (auto gpu_result =
                        Kernel::template call_result<MinimizerT>(
                            params, query_seq_, window_size, kmer_size))
                {
                    if (proof_dst == nullptr
                        || !gpu_result->dispatch_token.accepted()) {
                        bioseq_log<ERR>(
                            "Vulkan minimizer stage lacks an operation token or proof sink");
                        std::abort();
                    }
                    PartialVulkanExecutionProof proof{};
                    proof.query_sketch_dispatches =
                        gpu_result->dispatch_token.completed_roundtrips;
                    proof_dst->record_partial_vulkan_execution_proof(proof);
                    auto device_slab = make_arena_slab<MinimizerT>();
                    device_slab.reserve(gpu_result->host_minimizers.size());
                    if (!gpu_result->host_minimizers.empty()) {
                        std::memcpy(device_slab.data(),
                                    gpu_result->host_minimizers.data(),
                                    gpu_result->host_minimizers.size() * sizeof(MinimizerT));
                    }
                    device_slab.resize(gpu_result->host_minimizers.size());
                    MinimizerBatchView<MinimizerT> batch{
                        .host = device_slab.as_const_span(),
                        .device = std::optional<
                            bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>>{
                                gpu_result->device_span()},
                    };
                    // The reservation owns the vector capacity, so destroy
                    // that capacity before releasing its permit. `batch.device`
                    // independently retains the resident device permit (§6.1).
                    std::vector<MinimizerT>{}.swap(
                        gpu_result->host_minimizers);
                    gpu_result->host_permit.reset();
                    return std::move(token).emit_minimizer_batch(batch);
                }
            }
        }

        auto sketches_slab = make_arena_slab<MinimizerT>();
        sketches_slab.reserve(detail::estimated_sketch_count(
            query_seq_.bp_size(), static_cast<int32_t>(window_size)));

        sketch_collector_arena_adapter<MinimizerT> collector{&sketches_slab};

        if constexpr (Kernel::emits_partial_vulkan_execution_proof) {
            const auto dispatch_token = [&] {
                if constexpr (!Cep::generic_fallback) {
                    return Kernel::template call_constexpr<
                        Cep::window_len, Cep::kmer_len, MinimizerT>(
                            params, collector, query_seq_);
                } else {
                    return Kernel::template call<MinimizerT>(
                        params, collector, query_seq_,
                        window_size, kmer_size);
                }
            }();
            if (proof_dst == nullptr || !dispatch_token.accepted()) {
                bioseq_log<ERR>(
                    "Vulkan minimizer stage returned no accepted execution token");
                std::abort();
            }
            PartialVulkanExecutionProof proof{};
            proof.query_sketch_dispatches =
                dispatch_token.completed_roundtrips;
            proof_dst->record_partial_vulkan_execution_proof(proof);
        } else if constexpr (Kernel::emits_scalar_minimizer_execution_proof) {
            ScalarMinimizerExecutionProof proof{};
            if constexpr (!Cep::generic_fallback) {
                proof = Kernel::template call_constexpr<
                    Cep::window_len, Cep::kmer_len, MinimizerT>(
                        params, collector, query_seq_);
            } else {
                proof = Kernel::template call<MinimizerT>(
                    params, collector, query_seq_,
                    window_size, kmer_size);
            }
            if (proof_dst == nullptr || !proof.accepted()) {
                bioseq_log<ERR>(
                    "Scalar minimizer stage returned rejected proof");
                std::abort();
            }
            proof_dst->record_scalar_query_minimizer_execution_proof(proof);
        } else if constexpr (Kernel::emits_exact_simd_execution_proof) {
            const auto proof = [&] {
                if constexpr (!Cep::generic_fallback) {
                    return Kernel::template call_constexpr<
                        Cep::window_len, Cep::kmer_len, MinimizerT>(
                            params, collector, query_seq_);
                } else {
                    return Kernel::template call<MinimizerT>(
                        params, collector, query_seq_,
                        window_size, kmer_size);
                }
            }();
            if (proof_dst == nullptr
                || !proof.accepted(
                    /*require_nonempty_vector_work=*/false)) {
                bioseq_log<ERR>(
                    "ExactSimd minimizer stage returned rejected proof");
                std::abort();
            }
            proof_dst->record_exact_simd_query_execution_proof(proof);
        } else if constexpr (Kernel::emits_paper_nthash_execution_proof) {
            const auto proof = [&] {
                if constexpr (!Cep::generic_fallback) {
                    return Kernel::template call_constexpr<
                        Cep::window_len, Cep::kmer_len, MinimizerT>(
                            params, collector, query_seq_);
                } else {
                    return Kernel::template call<MinimizerT>(
                        params, collector, query_seq_,
                        window_size, kmer_size);
                }
            }();
            if (proof_dst == nullptr
                || !proof.accepted(
                    /*require_nonempty_vector_work=*/false)) {
                bioseq_log<ERR>(
                    "PaperNtHashSimd minimizer stage returned rejected proof");
                std::abort();
            }
            proof_dst->record_paper_nthash_query_execution_proof(proof);
        } else if constexpr (!Cep::generic_fallback) {
            Kernel::template call_constexpr<
                Cep::window_len, Cep::kmer_len, MinimizerT>(
                    params, collector, query_seq_);
        } else {
            Kernel::template call<MinimizerT>(
                params, collector, query_seq_, window_size, kmer_size);
        }

        if (host_timing_dst != nullptr) {
            host_timing_dst->record_query_sketch_host_time(
                elapsed_host_stage_ns(host_stage_started));
        }
        return std::move(token).emit_batch(std::span<const MinimizerT>{
            sketches_slab.data(), sketches_slab.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = std::string{Kernel::algorithm_label()};
        Kernel::populate_runtime_params({}, algo.parameters);
        algo.computed_properties = Kernel::template static_properties<Cep>();
        desc.algorithm = std::move(algo);
        // Reserve target for the sketch arena slab (DOC0249 §21.4
        // "estimated_sketch_count").  `query_seq_` is a borrowed pointer; on
        // the describe path the proto is empty so bp_size() is zero — which
        // still yields the +64 safety margin from `estimated_sketch_count`.
        desc.estimated_batch_size = detail::estimated_sketch_count(
            query_seq_.bp_size(),
            runtime_w > 0 ? runtime_w : 10);
        return desc;
    }
};

/**
 * @brief Stage 1: pass-through that decorates downstream with TargetIndex&.
 *
 * Converts the sink shape from `on_batch(span<MinimizerT>)` upstream to
 * `on_batch_with_index(span<MinimizerT>, IndexT const&)` downstream.  Inlines
 * away at -O3 (zero runtime cost).
 */
template <class IndexT>
struct IndexStage : StageBase<IndexStage<IndexT>, IndexT /* pass-through; downstream sees the same sketch span */> {
    static constexpr std::string_view stage_name = "IndexStage";

    const IndexT* index_;
    bioseq::indexing::GpuIndexCacheIdentity identity_{};

    template <is_constexpr_aligner_params Cep, class B, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process(B&& sketches, EmitToken<Sink>&& token) {
        // DOC0307 §5.2 / DOC0351 — whole in-memory targets keep pointer
        // identity (`{}`); segmented recompute/assembly passes a nonzero
        // identity so a reused stack index object cannot alias another segment
        // in the GPU resident cache.
        if constexpr (is_minimizer_batch_view_v<B>) {
            return std::move(token).emit_minimizer_batch_with_index(
                std::forward<B>(sketches), *index_, identity_);
        } else {
            using ElemT = std::remove_const_t<
                typename std::remove_cvref_t<B>::value_type>;
            return std::move(token).emit_batch_with_index(
                std::span<const ElemT>{sketches.data(), sketches.size()}, *index_, identity_);
        }
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        desc.extra_description = "target index lookup hand-off";
        return desc;
    }
};

enum class SourceDirectHostAuthority : std::uint8_t {
    none,
    canonical_empty,
    reflected_process_footprint,
    retained_exact_permit,
};

/// One operation-scoped ledger for the selected source-direct resident route.
/// It emits exactly one planning receipt after sort/count and exactly one
/// terminal receipt after cache/fence/arena cleanup (or unwind).  Checksums are
/// allocation-integrity evidence only; biological parity remains test/bench
/// evidence and is deliberately absent from these receipts.
struct DirectResidentOperationLedger {
    std::uint64_t operation_identity = 0u;
    std::uint64_t minimizer_count = 0u;
    std::uint64_t distinct_count = 0u;
    std::uint64_t logical_control_bytes = 0u;
    std::uint64_t logical_position_bytes = 0u;
    std::uint64_t logical_resident_bytes = 0u;
    std::uint64_t control_requirement_bytes = 0u;
    std::uint64_t position_requirement_bytes = 0u;
    std::uint64_t control_allowance_bytes = 0u;
    std::uint64_t position_allowance_bytes = 0u;
    std::uint64_t conservative_allowance_bytes = 0u;
    std::uint64_t conservative_request_bytes = 0u;
    std::uint64_t reserved_before_bytes = 0u;
    std::uint64_t allocatable_before_bytes = 0u;
    std::uint64_t margin_bytes = 0u;
    std::uint64_t shortfall_bytes = 0u;
    std::uint64_t max_storage_buffer_range = 0u;
    std::uint64_t source_capacity_bytes = 0u;
    std::uint64_t source_reflected_bytes = 0u;
    std::uint64_t source_retained_permit_bytes = 0u;
    std::uint64_t source_releases = 0u;
    std::uint64_t source_released_bytes = 0u;
    std::uint64_t cache_preinsertions = 0u;
    std::uint64_t cache_required_lookups = 0u;
    std::uint64_t cache_required_hits = 0u;
    std::uint64_t mapped_prefix_passes = 0u;
    std::uint64_t mapped_prefix_lookups = 0u;
    std::uint64_t mapped_prefix_comparisons = 0u;
    std::uint64_t expected_seed_consumers = 0u;
    std::uint64_t completed_seed_consumers = 0u;
    std::uint64_t slab_capacity_before = 0u;
    std::uint64_t slab_requested_bytes = 0u;
    std::uint64_t slab_actual_bytes = 0u;
    std::uint64_t slab_retained_permit_bytes = 0u;
    std::uint64_t slab_permit_releases = 0u;
    std::uint64_t seed_reserved_bytes = 0u;
    std::uint64_t seed_planned_vulkan_bytes = 0u;
    std::uint64_t seed_actual_vulkan_bytes = 0u;
    std::uint64_t seed_roundtrips = 0u;
    std::uint64_t zero_query_bypasses = 0u;
    std::uint64_t zero_anchor_bypasses = 0u;
    std::uint64_t seed_fence_completions = 0u;
    std::uint64_t seed_dispatches = 0u;
    std::uint64_t cache_cleanup_releases = 0u;
    std::uint64_t resident_cleanup_bytes = 0u;
    std::uint64_t cpu_index_releases = 0u;
    std::uint64_t cpu_position_bytes_released = 0u;
    std::uint64_t planning_receipts = 0u;
    std::uint64_t terminal_receipts = 0u;
    bioseq::indexing::GpuSourceDirectMaterializationProof materialization{};
    SourceDirectHostAuthority source_authority =
        SourceDirectHostAuthority::none;
    bool phase_c_checked = false;
    bool phase_c_compatible = false;
    bool source_authority_checked = false;
    bool source_authority_accepted = false;
    bool runtime_checked = false;
    bool runtime_available = false;
    bool hardware_checked = false;
    bool hardware_available = false;
    bool sorted_shape_checked = false;
    bool sorted_shape_accepted = false;
    bool identity_checked = false;
    bool identity_accepted = false;
    bool u32_shape_checked = false;
    bool u32_shape_accepted = false;
    bool storage_ranges_checked = false;
    bool control_range_accepted = false;
    bool position_range_accepted = false;
    bool mapping_plan_checked = false;
    bool mapping_plan_accepted = false;
    bool admission_snapshot_taken = false;
    bool canonical_empty = false;
    bool planning_accepted = false;
    bool route_committed = false;
    bool materialization_started = false;
    bool materialization_completed = false;
    bool resident_published = false;
    bool source_release_completed = false;
    bool source_released_after_publication = false;
    bool empty_bypass = false;
    bool cache_lookup_started = false;
    bool mapped_prefix_started = false;
    bool slab_admission_started = false;
    bool seed_dispatch_started = false;
    bool pipeline_started = false;
    bool pipeline_completed = false;
    bool planning_log_failed = false;
    bool terminal_success = false;

    [[nodiscard]] constexpr std::string_view route_name() const noexcept {
        return canonical_empty
            ? "empty_no_resident_zero_anchor"
            : "full_sorted_dense_direct_mapped";
    }

    [[nodiscard]] static constexpr std::string_view check_status(
        bool checked, bool accepted) noexcept
    {
        if (!checked) return "not_reached";
        return accepted ? "accepted" : "rejected";
    }

    [[nodiscard]] static constexpr std::string_view stage_status(
        bool checked, bool accepted, bool bypass) noexcept
    {
        return bypass ? "bypass" : check_status(checked, accepted);
    }

    [[nodiscard]] static constexpr std::string_view source_authority_name(
        SourceDirectHostAuthority authority) noexcept
    {
        switch (authority) {
            case SourceDirectHostAuthority::canonical_empty:
                return "canonical_empty";
            case SourceDirectHostAuthority::reflected_process_footprint:
                return "reflected_process_footprint";
            case SourceDirectHostAuthority::retained_exact_permit:
                return "retained_exact_permit";
            case SourceDirectHostAuthority::none:
                return "none";
        }
        return "unknown";
    }

    [[nodiscard]] static constexpr std::string_view resident_authority_name(
        bioseq::indexing::GpuResidentAccountingAuthority authority) noexcept
    {
        switch (authority) {
            case bioseq::indexing::GpuResidentAccountingAuthority::
                    reflected_process_footprint:
                return "reflected_process_footprint";
            case bioseq::indexing::GpuResidentAccountingAuthority::
                    retained_exact_permit:
                return "retained_exact_permit";
            case bioseq::indexing::GpuResidentAccountingAuthority::none:
                return "none";
        }
        return "unknown";
    }

    void log_planning(
        std::string_view decision, std::string_view reason) noexcept
    {
        if (planning_receipts != 0u) {
            planning_log_failed = true;
            return;
        }
        try {
            bioseq_log<INF>(
                "[gpu_source_direct_operation_planning] operation={} "
                "route={} "
                "source_authority={} source_authority_status={} "
                "phase_c_status={} runtime_status={} device_status={} "
                "sorted_shape_status={} abi_identity_status={} "
                "u32_shape_status={} control_range_status={} "
                "position_range_status={} mapping_plan_status={} "
                "N={} D={} control_bytes={} position_bytes={} "
                "logical_bytes={} control_requirement_bytes={} "
                "position_requirement_bytes={} control_allowance_bytes={} "
                "position_allowance_bytes={} allowance_bytes={} "
                "conservative_request_bytes={} reserved_before_bytes={} "
                "allocatable_before_bytes={} margin_bytes={} "
                "shortfall_bytes={} max_storage_buffer_range={} "
                "canonical_empty_bypass={} gate_enabled=0 "
                "decision={} reason={}",
                operation_identity,
                route_name(),
                source_authority_name(source_authority),
                check_status(source_authority_checked,
                             source_authority_accepted),
                check_status(phase_c_checked, phase_c_compatible),
                check_status(runtime_checked, runtime_available),
                check_status(hardware_checked, hardware_available),
                check_status(sorted_shape_checked, sorted_shape_accepted),
                check_status(identity_checked, identity_accepted),
                check_status(u32_shape_checked, u32_shape_accepted),
                check_status(storage_ranges_checked,
                             control_range_accepted),
                check_status(storage_ranges_checked,
                             position_range_accepted),
                check_status(mapping_plan_checked,
                             mapping_plan_accepted),
                minimizer_count,
                distinct_count,
                logical_control_bytes,
                logical_position_bytes,
                logical_resident_bytes,
                control_requirement_bytes,
                position_requirement_bytes,
                control_allowance_bytes,
                position_allowance_bytes,
                conservative_allowance_bytes,
                conservative_request_bytes,
                reserved_before_bytes,
                allocatable_before_bytes,
                margin_bytes,
                shortfall_bytes,
                max_storage_buffer_range,
                canonical_empty ? 1 : 0,
                decision,
                reason);
            planning_receipts = 1u;
        } catch (...) {
            planning_log_failed = true;
        }
    }

    [[nodiscard]] bool terminal_success_invariants() const noexcept {
        const bool explicit_pipeline_bypass =
            expected_seed_consumers == 0u
            && completed_seed_consumers == 0u
            && (canonical_empty
                ? empty_bypass && zero_query_bypasses == 0u
                : zero_query_bypasses == 1u);
        const bool pipeline_exact = explicit_pipeline_bypass
            ? !pipeline_started && !pipeline_completed
            : pipeline_started && pipeline_completed;
        if (planning_receipts != 1u || planning_log_failed
            || !planning_accepted || !route_committed
            || !source_release_completed
            || !pipeline_exact
            || completed_seed_consumers != expected_seed_consumers
            || cpu_index_releases != 0u
            || cpu_position_bytes_released != 0u) {
            return false;
        }
        if (canonical_empty) {
            return empty_bypass
                && !materialization_started
                && !materialization_completed
                && !resident_published
                && minimizer_count == 0u
                && distinct_count == 0u
                && logical_control_bytes == 0u
                && logical_position_bytes == 0u
                && logical_resident_bytes == 0u
                && control_requirement_bytes == 0u
                && position_requirement_bytes == 0u
                && control_allowance_bytes == 0u
                && position_allowance_bytes == 0u
                && conservative_allowance_bytes == 0u
                && conservative_request_bytes == 0u
                && source_capacity_bytes == 0u
                && source_reflected_bytes == 0u
                && source_retained_permit_bytes == 0u
                && source_releases == 0u
                && source_released_bytes == 0u
                && !source_released_after_publication
                && cache_preinsertions == 0u
                && cache_required_lookups == 0u
                && cache_required_hits == 0u
                && mapped_prefix_passes == 0u
                && mapped_prefix_lookups == 0u
                && mapped_prefix_comparisons == 0u
                && slab_capacity_before == 0u
                && slab_requested_bytes == 0u
                && slab_actual_bytes == 0u
                && slab_retained_permit_bytes == 0u
                && slab_permit_releases == 0u
                && seed_reserved_bytes == 0u
                && seed_planned_vulkan_bytes == 0u
                && seed_actual_vulkan_bytes == 0u
                && seed_roundtrips == 0u
                && zero_anchor_bypasses == 0u
                && seed_fence_completions == 0u
                && seed_dispatches == 0u
                && !pipeline_started
                && cache_cleanup_releases == 0u
                && resident_cleanup_bytes == 0u;
        }
        if (!materialization_started || !materialization_completed
            || !resident_published || empty_bypass) {
            return false;
        }
        const std::uint64_t allocation_sum =
            bioseq::parallelism::saturating_add(
                materialization.actual_slot_allocation_bytes,
                materialization.actual_position_allocation_bytes);
        const bool exact_materialization = allocation_sum
                == materialization.actual_allocation_bytes
            && allocation_sum
                != (std::numeric_limits<std::uint64_t>::max)()
            && materialization.logical_slot_bytes
                == logical_control_bytes
            && materialization.logical_position_bytes
                == logical_position_bytes
            && materialization.logical_resident_bytes
                == logical_resident_bytes
            && materialization.control_allocation_requirement_bytes
                == control_requirement_bytes
            && materialization.position_allocation_requirement_bytes
                == position_requirement_bytes
            && materialization.control_allocation_allowance_bytes
                == control_allowance_bytes
            && materialization.position_allocation_allowance_bytes
                == position_allowance_bytes
            && materialization.conservative_allowance_bytes
                == conservative_allowance_bytes
            && materialization.conservative_request_bytes
                == conservative_request_bytes
            && materialization.actual_slot_allocation_bytes
                == control_requirement_bytes
            && materialization.actual_position_allocation_bytes
                == position_requirement_bytes
            && materialization.actual_allocation_bytes
                == conservative_request_bytes
            && materialization.fully_touched_bytes
                == materialization.actual_allocation_bytes
            && materialization.permit_reconciliations == 1u;
        const bool reflected = materialization.authority
                == bioseq::indexing::GpuResidentAccountingAuthority::
                    reflected_process_footprint
            && materialization.reflected_footprint_bytes
                >= materialization.actual_allocation_bytes
            && materialization.retained_permit_bytes == 0u;
        const bool retained = materialization.authority
                == bioseq::indexing::GpuResidentAccountingAuthority::
                    retained_exact_permit
            && materialization.retained_permit_bytes
                == materialization.actual_allocation_bytes
            && materialization.reflected_footprint_bytes == 0u;
        const bool slab_exact = slab_capacity_before == 0u
            && slab_requested_bytes == slab_actual_bytes
            && slab_actual_bytes == slab_retained_permit_bytes
            && (slab_actual_bytes == 0u
                ? slab_permit_releases == 0u
                : slab_permit_releases == 1u);
        const bool zero_query = zero_query_bypasses == 1u;
        const bool seed_memory_exact = zero_query
            ? seed_reserved_bytes == 0u
                && seed_planned_vulkan_bytes == 0u
                && seed_actual_vulkan_bytes == 0u
            : slab_actual_bytes == 0u
            ? seed_reserved_bytes == 0u
                && seed_planned_vulkan_bytes == 0u
                && seed_actual_vulkan_bytes == 0u
            : seed_reserved_bytes != 0u
                && seed_planned_vulkan_bytes != 0u
                && seed_planned_vulkan_bytes
                    == seed_actual_vulkan_bytes;
        const bool zero_anchor = !zero_query
            && seed_roundtrips != 0u
            && zero_anchor_bypasses == seed_roundtrips;
        const bool lookup_exact = zero_query
            ? cache_required_hits == cache_required_lookups
                && cache_required_lookups == completed_seed_consumers
                && mapped_prefix_passes == 0u
            : cache_required_lookups >= 1u
                && cache_required_hits == cache_required_lookups
                && cache_required_lookups == completed_seed_consumers
                && mapped_prefix_passes
                    == 2u * cache_required_lookups;
        const bool seed_completion_exact = seed_roundtrips
                == cache_required_lookups
            && (zero_query
                ? slab_actual_bytes == 0u
                    && slab_requested_bytes == 0u
                    && zero_anchor_bypasses == 0u
                    && seed_dispatches == 0u
                    && seed_fence_completions == 0u
                : slab_actual_bytes == 0u
                ? zero_anchor_bypasses == seed_roundtrips
                    && seed_dispatches == 0u
                    && seed_fence_completions == 0u
                : zero_anchor_bypasses == 0u
                    && seed_dispatches == seed_roundtrips
                    && seed_fence_completions == seed_roundtrips);
        return exact_materialization
            && source_releases == 1u
            && source_released_bytes == source_capacity_bytes
            && source_released_after_publication
            && cache_preinsertions == 1u
            && lookup_exact
            && (reflected != retained)
            && slab_exact
            && seed_memory_exact
            && seed_completion_exact
            && cache_cleanup_releases == 1u
            && resident_cleanup_bytes
                == materialization.actual_allocation_bytes
            && materialization.source_checksum != 0u
            && materialization.source_checksum
                == materialization.mapped_checksum
            && materialization.complete_source_traversals == 1u
            && materialization.complete_mapped_traversals == 1u;
    }

    void log_terminal(
        std::string_view status,
        std::string_view reason) noexcept {
        if (terminal_receipts != 0u) return;
        const bool invariant_success = terminal_success_invariants();
        const bool accepted = status == "success" && invariant_success;
        const bool explicit_pipeline_bypass =
            expected_seed_consumers == 0u
            && completed_seed_consumers == 0u
            && (canonical_empty
                ? empty_bypass && zero_query_bypasses == 0u
                : zero_query_bypasses == 1u);
        const bool zero_anchor_bypass = seed_roundtrips != 0u
            && zero_anchor_bypasses == seed_roundtrips;
        const bool mapped_prefix_bypass = explicit_pipeline_bypass;
        const bool cleanup_checked = source_release_completed
            || cache_cleanup_releases != 0u
            || resident_cleanup_bytes != 0u;
        const bool cleanup_accepted = source_release_completed
            && (!resident_published
                || (cache_cleanup_releases == 1u
                    && resident_cleanup_bytes
                        == materialization.actual_allocation_bytes));
        try {
            bioseq_log<INF>(
            "[gpu_source_direct_operation_terminal] operation={} status={} "
            "reason={} route={} N={} D={} actual_control_allocation_bytes={} "
            "actual_position_allocation_bytes={} actual_allocation_bytes={} "
            "permit_reconciliations={} resident_authority={} "
            "reflected_bytes={} retained_permit_bytes={} fully_touched_bytes={} "
            "source_checksum={} mapped_checksum={} source_traversals={} "
            "mapped_traversals={} source_releases={} source_released_bytes={} "
            "source_released_after_publication={} cache_preinsertions={} "
            "required_lookups={} required_hits={} required_misses={} "
            "mapped_prefix_passes={} "
            "mapped_prefix_lookups={} mapped_prefix_comparisons={} "
            "slab_before={} slab_requested_bytes={} slab_actual_bytes={} "
            "slab_retained_permit_bytes={} slab_permit_releases={} "
            "seed_reserved_bytes={} seed_planned_vulkan_bytes={} "
            "seed_actual_vulkan_bytes={} "
            "expected_consumers={} completed_consumers={} seed_roundtrips={} "
            "seed_dispatches={} seed_fence_completions={} "
            "zero_query_bypasses={} zero_anchor_bypasses={} empty_bypass={} "
            "materialization_status={} publication_status={} "
            "cache_lookup_status={} mapped_prefix_status={} "
            "slab_admission_status={} seed_dispatch_status={} "
            "seed_fence_status={} cleanup_status={} pipeline_status={} "
            "pipeline_completed={} terminal_invariants={} "
            "cache_cleanup_releases={} resident_cleanup_bytes={}",
            operation_identity,
            accepted ? "success" : "failure",
            reason,
            route_name(),
            minimizer_count,
            distinct_count,
            materialization.actual_slot_allocation_bytes,
            materialization.actual_position_allocation_bytes,
            materialization.actual_allocation_bytes,
            materialization.permit_reconciliations,
            resident_authority_name(materialization.authority),
            materialization.reflected_footprint_bytes,
            materialization.retained_permit_bytes,
            materialization.fully_touched_bytes,
            materialization.source_checksum,
            materialization.mapped_checksum,
            materialization.complete_source_traversals,
            materialization.complete_mapped_traversals,
            source_releases,
            source_released_bytes,
            source_released_after_publication ? 1 : 0,
            cache_preinsertions,
            cache_required_lookups,
            cache_required_hits,
            cache_required_lookups >= cache_required_hits
                ? cache_required_lookups - cache_required_hits
                : 0u,
            mapped_prefix_passes,
            mapped_prefix_lookups,
            mapped_prefix_comparisons,
            slab_capacity_before,
            slab_requested_bytes,
            slab_actual_bytes,
            slab_retained_permit_bytes,
            slab_permit_releases,
            seed_reserved_bytes,
            seed_planned_vulkan_bytes,
            seed_actual_vulkan_bytes,
            expected_seed_consumers,
            completed_seed_consumers,
            seed_roundtrips,
            seed_dispatches,
            seed_fence_completions,
            zero_query_bypasses,
            zero_anchor_bypasses,
            empty_bypass ? 1 : 0,
            stage_status(
                materialization_started,
                materialization_completed,
                canonical_empty),
            stage_status(
                materialization_completed,
                resident_published,
                canonical_empty),
            stage_status(
                cache_lookup_started,
                cache_required_lookups != 0u
                    && cache_required_hits == cache_required_lookups,
                explicit_pipeline_bypass),
            stage_status(
                mapped_prefix_started,
                mapped_prefix_passes == 2u * cache_required_lookups,
                mapped_prefix_bypass),
            stage_status(
                slab_admission_started,
                slab_requested_bytes == slab_actual_bytes
                    && slab_actual_bytes == slab_retained_permit_bytes,
                explicit_pipeline_bypass || zero_anchor_bypass),
            stage_status(
                seed_dispatch_started,
                seed_dispatches != 0u
                    && seed_dispatches == seed_roundtrips,
                explicit_pipeline_bypass || zero_anchor_bypass),
            stage_status(
                seed_dispatch_started,
                seed_fence_completions == seed_roundtrips,
                explicit_pipeline_bypass || zero_anchor_bypass),
            check_status(cleanup_checked, cleanup_accepted),
            stage_status(
                pipeline_started,
                pipeline_completed,
                explicit_pipeline_bypass),
            pipeline_completed ? 1 : 0,
            invariant_success ? 1 : 0,
            cache_cleanup_releases,
            resident_cleanup_bytes);
            terminal_receipts = 1u;
            terminal_success = accepted;
        } catch (...) {
            terminal_success = false;
        }
    }
};

/// Last-resort receipt guard for failures before the wrapper's catch handler
/// can finish its unwind. Cleanup remains owned by the wrapper/PostSeed; this
/// guard only prevents a selected operation from disappearing without its two
/// operation-level receipts.
struct DirectResidentReceiptGuard {
    DirectResidentOperationLedger* ledger = nullptr;

    ~DirectResidentReceiptGuard() noexcept {
        if (ledger == nullptr) return;
        if (ledger->planning_receipts == 0u) {
            ledger->log_planning("reject", "unwind_before_planning_complete");
        }
        if (ledger->terminal_receipts == 0u) {
            ledger->log_terminal("failure", "unhandled_operation_unwind");
        }
    }
};

/// Release authority for one operation-owned transient CPU target index.
/// Ordinary transient indexes carry only `index`; the fixed Vulkan handoff
/// also carries the exact permit that accounts for storage not already
/// represented by its one-shot process-footprint authority.
struct TransientTargetIndexReleaseAuthority {
    bioseq::indexing::MinimizerHashmapFragmentIndex* index = nullptr;
    bioseq::parallelism::MemoryPermit* permit = nullptr;
    std::uint64_t expected_permit_bytes = 0u;
    bool require_exact_permit_bytes = false;
    bioseq::indexing::GpuMinimizerIndexCache*
        operation_owned_gpu_cache = nullptr;
    DirectResidentOperationLedger* direct_ledger = nullptr;

    [[nodiscard]] bool accepted() const noexcept {
        return index != nullptr
            && (!require_exact_permit_bytes || permit != nullptr)
            && (permit == nullptr
                || static_cast<bool>(*permit)
                    && (!require_exact_permit_bytes
                        || permit->bytes() == expected_permit_bytes));
    }
};

/// One operation-owned lifetime boundary between seed lookup and chaining.
/// Borrowed persisted CPU indexes may use it only to retire an invocation-owned
/// GPU cache; external caches never receive release authority. Every successful
/// seed consumer signals exactly once after dropping its local resident handle;
/// the last consumer deallocates any transient CPU lookup index and clears any
/// internally-owned GPU cache before downstream chaining begins.
struct PostSeedOwnedLifetime {
    struct ReleaseProof {
        std::uint64_t gpu_cache_evictions = 0;
        std::uint64_t gpu_resident_bytes_released = 0;
        std::uint64_t cpu_index_releases = 0;
        std::uint64_t cpu_position_bytes_released = 0;
        std::uint64_t cpu_index_permit_releases = 0;
        std::uint64_t cpu_index_permit_bytes_released = 0;
        bool cpu_index_storage_released_before_permit = false;
    };

    std::atomic<std::size_t> remaining_seed_users{0};
    bioseq::indexing::GpuMinimizerIndexCache* owned_gpu_cache = nullptr;
    TransientTargetIndexReleaseAuthority owned_cpu_index{};
    bioseq::parallelism::MemoryPermit retained_seed_arena_permit{};

    PostSeedOwnedLifetime(
            std::size_t seed_users,
            bioseq::indexing::GpuMinimizerIndexCache* gpu_cache,
            TransientTargetIndexReleaseAuthority cpu_index) noexcept
        : remaining_seed_users(seed_users),
          owned_gpu_cache(gpu_cache),
          owned_cpu_index(cpu_index) {}

    ~PostSeedOwnedLifetime() noexcept {
        if (owned_cpu_index.direct_ledger == nullptr) return;
        auto& ledger = *owned_cpu_index.direct_ledger;
        if (owned_gpu_cache != nullptr) {
            const std::uint64_t resident_bytes =
                owned_gpu_cache->resident_bytes();
            owned_gpu_cache->clear();
            owned_gpu_cache = nullptr;
            if (resident_bytes != 0u) {
                ledger.cache_cleanup_releases = 1u;
                ledger.resident_cleanup_bytes = resident_bytes;
            }
        }
        if (retained_seed_arena_permit) {
            ledger.slab_retained_permit_bytes =
                retained_seed_arena_permit.bytes();
            retained_seed_arena_permit.release();
            ledger.slab_permit_releases = 1u;
        }
        const bool users_completed =
            remaining_seed_users.load(std::memory_order_relaxed) == 0u;
        const bool accepted = users_completed
            && ledger.terminal_success_invariants();
        ledger.log_terminal(
            accepted ? "success" : "failure",
            accepted
                ? ledger.expected_seed_consumers == 0u
                    ? "completed_explicit_zero_consumer_bypass"
                    : "completed_after_last_seed_fence_and_arena_unwind"
                : users_completed
                    ? "route_specific_terminal_invariant_failed"
                    : "seed_user_not_completed");
    }

    PostSeedOwnedLifetime(const PostSeedOwnedLifetime&) = delete;
    PostSeedOwnedLifetime& operator=(const PostSeedOwnedLifetime&) = delete;

    [[nodiscard]] ReleaseProof complete_seed_user() {
        std::size_t previous =
            remaining_seed_users.load(std::memory_order_acquire);
        while (previous != 0u
               && !remaining_seed_users.compare_exchange_weak(
                   previous,
                   previous - 1u,
                   std::memory_order_acq_rel,
                   std::memory_order_acquire)) {
        }
        if (previous == 0u) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "post-seed lifetime user underflow"};
        }
        if (owned_cpu_index.direct_ledger != nullptr) {
            auto& ledger = *owned_cpu_index.direct_ledger;
            ledger.completed_seed_consumers =
                bioseq::parallelism::saturating_add(
                    ledger.completed_seed_consumers, 1u);
        }
        if (previous != 1u) return {};

        ReleaseProof proof{};
        if (owned_gpu_cache != nullptr) {
            auto* const cache_to_release = owned_gpu_cache;
            owned_gpu_cache = nullptr;
            proof.gpu_resident_bytes_released =
                cache_to_release->resident_bytes();
            cache_to_release->clear();
            proof.gpu_cache_evictions =
                proof.gpu_resident_bytes_released != 0u ? 1u : 0u;
            if (owned_cpu_index.direct_ledger != nullptr) {
                auto& ledger = *owned_cpu_index.direct_ledger;
                ledger.cache_cleanup_releases =
                    proof.gpu_cache_evictions;
                ledger.resident_cleanup_bytes =
                    proof.gpu_resident_bytes_released;
            }
        }
        if (owned_cpu_index.index != nullptr) {
            if (!owned_cpu_index.accepted()) {
                bioseq_log<ERR>(
                    "post-seed lifetime has invalid CPU index release authority");
                std::abort();
            }
            const bool had_resident_storage =
                !owned_cpu_index.index->table_empty()
                || owned_cpu_index.index->positions.capacity() != 0u;
            proof.cpu_position_bytes_released =
                bioseq::parallelism::saturating_mul(
                    owned_cpu_index.index->positions.capacity(),
                    sizeof(std::uint64_t));
            owned_cpu_index.index->release_storage();
            proof.cpu_index_releases = had_resident_storage ? 1u : 0u;
            if (owned_cpu_index.permit != nullptr) {
                if (!owned_cpu_index.index->table_empty()
                    || owned_cpu_index.index->positions.capacity() != 0u) {
                    bioseq_log<ERR>(
                        "post-seed CPU index permit release preceded storage");
                    std::abort();
                }
                proof.cpu_index_storage_released_before_permit = true;
                proof.cpu_index_permit_bytes_released =
                    owned_cpu_index.permit->bytes();
                proof.cpu_index_permit_releases =
                    static_cast<bool>(*owned_cpu_index.permit) ? 1u : 0u;
                owned_cpu_index.permit->release();
            }
        }
        return proof;
    }
};

/**
 * @brief Stage 2: seed-hit collection.
 *
 * Wraps the `collect_seed_hits` kernel, which writes its anchor output
 * directly into a caller-provided ArenaSlab (DOC0249 §13 follow-up:
 * output-side kernel-to-arena refactor).  No `std::vector` is materialised
 * inside the stage — the per-stage memcpy that the previous shape required
 * is eliminated, so the kernel's `push_back` traffic lands straight in the
 * pipeline arena.
 */
struct SeedHitStage : StageBase<SeedHitStage, ::bioseq::chaining::Anchor> {
    static constexpr std::string_view stage_name = "SeedHitStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    sequence_size_t                               query_len;
    // DOC0307 §5.2 — batch/segment-wave-scoped GPU index cache. The outer
    // wave creates one cache, passes `&cache` here, and clears it at wave end.
    // Null on single-shot paths (then the GPU seed path uploads uncached).
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr;
    MatchResultCollection* proof_dst = nullptr;
    PostSeedOwnedLifetime* post_seed_owned_lifetime = nullptr;
    MatchResultCollection* host_timing_dst = nullptr;

    // DOC0307 §5.2 — host/device accessors for both the std::span CPU shape and
    // the MinimizerBatchView device shape.
    template <class MinimizerT>
    [[nodiscard, gnu::always_inline]] static std::span<const MinimizerT>
    sketch_span_of(const std::span<const MinimizerT>& s) noexcept { return s; }
    template <class MinimizerT>
    [[nodiscard, gnu::always_inline]] static std::span<const MinimizerT>
    sketch_span_of(const MinimizerBatchView<MinimizerT>& b) noexcept { return b.host; }

    template <class MinimizerT>
    [[nodiscard, gnu::always_inline]] static
    std::optional<bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>>
    device_span_of(const std::span<const MinimizerT>&) noexcept { return std::nullopt; }
    template <class MinimizerT>
    [[nodiscard, gnu::always_inline]] static
    std::optional<bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>>
    device_span_of(const MinimizerBatchView<MinimizerT>& b) noexcept { return b.device; }

    template <is_constexpr_aligner_params Cep, class B, class IndexT, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_with_index(
        B&& sketches, const IndexT& target_index_ref,
        bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity,
        EmitToken<Sink>&& token)
    {
        const std::uint64_t host_stage_started = host_timing_dst == nullptr
            ? 0u : host_stage_now_ns();
        const int32_t kmer_size = Cep::generic_fallback
            ? params.kmer_size
            : static_cast<int32_t>(Cep::kmer_len);

        using MinimizerT = std::remove_const_t<
            typename std::remove_cvref_t<B>::value_type>;

        auto sketch_span = sketch_span_of<MinimizerT>(sketches);
        auto device_span = device_span_of<MinimizerT>(sketches);
        DirectResidentOperationLedger* const direct_ledger =
            post_seed_owned_lifetime == nullptr
            ? nullptr
            : post_seed_owned_lifetime->owned_cpu_index.direct_ledger;

        // DOC0307 §5.2 — run the selected GPU seed-hit lookup before the CPU
        // path. If it declines, the selected Vulkan pipeline fails loudly.
        // Only the concrete MinimizerHashmapFragmentIndex carries the GPU view.
        if (params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
            bool seed_gpu_declined = false;
            bioseq::gpu::vulkan::VulkanSeedLookupDecline seed_decline{
                .query_length = static_cast<std::uint64_t>(query_len),
                .query_minimizer_count = static_cast<std::uint64_t>(
                    sketch_span.size()),
                .kmer_size = kmer_size,
                .max_occ = params.max_occ,
                .fused_frontend = params.vulkan_fused_frontend_enabled,
            };
            if constexpr (std::same_as<std::remove_cvref_t<IndexT>,
                          bioseq::indexing::MinimizerHashmapFragmentIndex>) {
                // Strict Vulkan selection owns its anchor allocation. A
                // prepared fragment view has no resident GPU index and reaches
                // the fail-closed decline below without allocating CPU output.
                auto anchors_slab = make_arena_slab<Anchor>();
                if (gpu_index_identity.source_direct_sorted_dense
                    && gpu_index_identity.canonical_empty) {
                    if (!bioseq::indexing::
                            gpu_source_direct_identity_well_formed(
                                gpu_index_identity)
                        || !target_index_ref.table_empty()
                        || !target_index_ref.positions.empty()
                        || gpu_index_identity.table_size != 0u
                        || gpu_index_identity.position_count != 0u
                        || gpu_index_cache != nullptr
                        || direct_ledger == nullptr
                        || params.vulkan_fused_frontend_enabled) {
                        throw bioseq::gpu::vulkan::VulkanRuntimeError{
                            "canonical empty source-direct route is invalid"};
                    }
                    direct_ledger->empty_bypass = true;
                    if (post_seed_owned_lifetime != nullptr
                        && post_seed_owned_lifetime->remaining_seed_users.load(
                               std::memory_order_acquire) != 0u) {
                        static_cast<void>(
                            post_seed_owned_lifetime->complete_seed_user());
                    }
                    return std::move(token).emit_batch(
                        std::span<Anchor>{});
                }
                auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
                if (bioseq::gpu::vulkan::hardware_device_available(runtime)) {
                    auto key = bioseq::indexing::make_gpu_index_cache_key(
                        target_index_ref, gpu_index_identity);
                    std::shared_ptr<const bioseq::indexing::
                        GpuResidentMinimizerIndex> resident_index;
                    // Keep operation-originated materialization evidence
                    // separate from the seed-decline record. The seed lookup
                    // entry point intentionally resets its decline output at
                    // entry, while this evidence must survive a successful
                    // lookup so the benchmark records the one real upload and
                    // leaves later cache hits at zero.
                    bioseq::indexing::GpuIndexUploadDecline
                        index_materialization{};
                    if (gpu_index_cache == nullptr) {
                        seed_decline.reason = bioseq::gpu::vulkan::
                            VulkanSeedLookupDeclineReason::index_cache_unavailable;
                    } else if (gpu_index_identity.
                            source_direct_sorted_dense) {
                        if (!bioseq::indexing::
                                gpu_source_direct_identity_well_formed(
                                    gpu_index_identity)
                            || gpu_index_identity.canonical_empty
                            || params.vulkan_fused_frontend_enabled
                            || direct_ledger == nullptr) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                                "source-direct resident seed identity is invalid"};
                        }
                        direct_ledger->cache_lookup_started = true;
                        direct_ledger->cache_required_lookups =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->cache_required_lookups, 1u);
                        auto required =
                            gpu_index_cache->find_required_preinserted(
                                runtime, key);
                        if (required) {
                            resident_index = std::move(*required);
                            direct_ledger->cache_required_hits =
                                bioseq::parallelism::saturating_add(
                                    direct_ledger->cache_required_hits, 1u);
                        } else {
                            seed_decline.reason = bioseq::gpu::vulkan::
                                VulkanSeedLookupDeclineReason::
                                    resident_index_unavailable;
                        }
                    } else {
                        resident_index = gpu_index_cache->get_or_upload(
                            runtime, target_index_ref, key,
                            params.vulkan_index_cache_enabled,
                            &index_materialization);
                        if (!resident_index) {
                            seed_decline.index_upload = index_materialization;
                            seed_decline.reason = index_materialization.declined()
                                ? bioseq::gpu::vulkan::
                                    VulkanSeedLookupDeclineReason::
                                        resident_index_upload_declined
                                : bioseq::gpu::vulkan::
                                    VulkanSeedLookupDeclineReason::
                                        resident_index_unavailable;
                        }
                    }
                    if (resident_index
                        && gpu_index_identity.source_direct_sorted_dense
                        && post_seed_owned_lifetime == nullptr) {
                        throw bioseq::gpu::vulkan::VulkanRuntimeError{
                            "source-direct seed route has no arena permit owner"};
                    }
                    if (direct_ledger != nullptr) {
                        if (post_seed_owned_lifetime->retained_seed_arena_permit) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                                "source-direct seed route retained a prior arena permit"};
                        }
                        if (proof_dst == nullptr) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                                "source-direct seed stage has no execution-proof sink"};
                        }
                        direct_ledger->mapped_prefix_started = true;
                        direct_ledger->slab_capacity_before =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->slab_capacity_before,
                                static_cast<std::uint64_t>(
                                    anchors_slab.capacity()));
                    }
                    bioseq::gpu::vulkan::VulkanSeedLookupCompletion
                        seed_completion{false};
                    if (resident_index) {
                        seed_completion = bioseq::gpu::vulkan::
                            collect_seed_hits_vulkan<MinimizerT>(
                                sketch_span,
                                device_span,
                                target_index_ref,
                                resident_index,
                                params.vulkan_enabled,
                                query_len,
                                params.max_occ,
                                kmer_size,
                                anchors_slab,
                                params.vulkan_fused_frontend_enabled,
                                &seed_decline);
                    }
                    if (direct_ledger != nullptr) {
                        direct_ledger->mapped_prefix_passes =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->mapped_prefix_passes,
                                seed_decline.mapped_prefix_authority_passes);
                        direct_ledger->mapped_prefix_lookups =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->mapped_prefix_lookups,
                                seed_decline.mapped_prefix_lookup_count);
                        direct_ledger->mapped_prefix_comparisons =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->mapped_prefix_comparisons,
                                seed_decline.mapped_prefix_comparison_count);
                        direct_ledger->slab_requested_bytes =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->slab_requested_bytes,
                                seed_decline.anchors_out_growth_bytes);
                        direct_ledger->slab_actual_bytes =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->slab_actual_bytes,
                                seed_decline.anchors_out_actual_growth_bytes);
                        direct_ledger->slab_retained_permit_bytes =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->slab_retained_permit_bytes,
                                seed_decline.
                                    anchors_out_retained_permit_bytes);
                        direct_ledger->seed_reserved_bytes =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->seed_reserved_bytes,
                                seed_decline.reserved_bytes);
                        direct_ledger->seed_planned_vulkan_bytes =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->seed_planned_vulkan_bytes,
                                seed_decline.
                                    planned_vulkan_allocation_bytes);
                        direct_ledger->seed_actual_vulkan_bytes =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->seed_actual_vulkan_bytes,
                                seed_decline.actual_vulkan_allocation_bytes);
                        direct_ledger->slab_admission_started =
                            seed_decline.exact_anchor_count != 0u;
                        direct_ledger->seed_dispatches =
                            bioseq::parallelism::saturating_add(
                                direct_ledger->seed_dispatches,
                                seed_decline.seed_lookup_dispatches);
                        direct_ledger->seed_dispatch_started =
                            direct_ledger->seed_dispatch_started
                            || seed_decline.seed_lookup_dispatches != 0u;
                        if (seed_completion) {
                            direct_ledger->seed_roundtrips =
                                bioseq::parallelism::saturating_add(
                                    direct_ledger->seed_roundtrips, 1u);
                            if (seed_decline.exact_anchor_count == 0u) {
                                direct_ledger->zero_anchor_bypasses =
                                    bioseq::parallelism::saturating_add(
                                        direct_ledger->zero_anchor_bypasses,
                                        1u);
                            }
                            if (seed_decline.seed_lookup_dispatches != 0u) {
                                direct_ledger->seed_fence_completions =
                                    bioseq::parallelism::saturating_add(
                                        direct_ledger->seed_fence_completions,
                                        1u);
                            }
                        }
                    }
                    if (seed_completion.retained_anchor_permit) {
                        if (post_seed_owned_lifetime == nullptr
                            || post_seed_owned_lifetime->
                                retained_seed_arena_permit) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                                "source-direct seed permit owner is invalid"};
                        }
                        post_seed_owned_lifetime->
                            retained_seed_arena_permit = std::move(
                                seed_completion.retained_anchor_permit);
                        if (seed_completion.retained_anchor_bytes
                            != post_seed_owned_lifetime->
                                retained_seed_arena_permit.bytes()) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                                "source-direct seed permit byte authority is invalid"};
                        }
                    }
                    if (resident_index && seed_completion) {
                        if (proof_dst == nullptr) {
                            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                                "Vulkan seed stage has no execution-proof sink"};
                        }
                        PartialVulkanExecutionProof proof{};
                        proof.seed_lookup_dispatches =
                            seed_decline.seed_lookup_dispatches;
                        proof.seed_open_addressed_lookup_dispatches =
                            seed_decline.open_addressed_lookup_dispatches;
                        proof.seed_sorted_dense_lookup_dispatches =
                            seed_decline.sorted_dense_lookup_dispatches;
                        proof.seed_bitonic_sort_dispatches =
                            seed_decline.sort_proof.bitonic_sort_dispatches;
                        proof.seed_radix_sort_operations =
                            seed_decline.sort_proof.radix_sort_operations;
                        proof.seed_radix_anchor_count =
                            seed_decline.sort_proof.radix_anchor_count;
                        proof.seed_radix_passes =
                            seed_decline.sort_proof.radix_passes;
                        proof.seed_radix_histogram_dispatches =
                            seed_decline.sort_proof.radix_histogram_dispatches;
                        proof.seed_radix_scan_dispatches =
                            seed_decline.sort_proof.radix_scan_dispatches;
                        proof.seed_radix_scatter_dispatches =
                            seed_decline.sort_proof.radix_scatter_dispatches;
                        proof.seed_host_sort_fallbacks =
                            seed_decline.sort_proof.host_sort_fallbacks;
                        if (seed_decline.canonical_anchor_order_checked) {
                            proof.seed_anchor_proof_operations = 1u;
                            proof.seed_canonical_anchor_count =
                                seed_decline.canonical_anchor_count;
                            proof.seed_canonical_anchor_order_checks = 1u;
                            proof.seed_canonical_anchor_order_accepts =
                                seed_decline.canonical_anchor_order_accepted
                                    ? 1u : 0u;
                            proof.seed_canonical_tie_normalization_runs =
                                seed_decline.
                                    canonical_tie_normalization_runs;
                            proof.seed_canonical_tie_normalization_anchors =
                                seed_decline.
                                    canonical_tie_normalization_anchors;
                            proof.seed_anchor_digest_lane0 =
                                seed_decline.canonical_anchor_digest_lo;
                            proof.seed_anchor_digest_lane1 =
                                seed_decline.canonical_anchor_digest_hi;
                        }
                        // `index_upload` is populated only by the operation
                        // that actually materialized a new resident index.
                        // Cache hits leave it zeroed, so these counters never
                        // multiply one upload by the number of queries reusing
                        // it.
                        const auto& index_upload = index_materialization;
                        if (index_upload.slot_capacity != 0u) {
                            proof.gpu_index_materializations = 1u;
                            if (index_upload.representation
                                == bioseq::indexing::GpuIndexRepresentation::
                                    open_addressed_70) {
                                proof.gpu_index_open_addressed_materializations = 1u;
                            } else {
                                proof.gpu_index_sorted_dense_materializations = 1u;
                            }
                            proof.gpu_index_unique_keys =
                                index_upload.unique_key_count;
                            proof.gpu_index_slot_capacity =
                                index_upload.slot_capacity;
                            proof.gpu_index_logical_resident_bytes =
                                index_upload.resident_bytes;
                            proof.gpu_index_actual_allocation_bytes =
                                index_upload.actual_allocation_bytes;
                            proof.gpu_index_fully_touched_bytes =
                                index_upload.fully_touched_bytes;
                            proof.gpu_index_staging_bytes =
                                index_upload.staging_bytes;
                            proof.gpu_index_permit_releases =
                                index_upload.materialization_permit_released ? 1u : 0u;
                            proof.gpu_index_process_footprint_reflections =
                                index_upload.process_footprint_reflected ? 1u : 0u;
                            proof.gpu_index_reflected_footprint_bytes =
                                index_upload.reflected_footprint_delta_bytes;
                            proof.gpu_index_max_load_ppm = index_upload.load_ppm;
                            proof.gpu_index_max_probe = index_upload.max_probe;
                        }
                        // `emit_batch` is synchronous: without dropping this
                        // local handle first, the multi-gigabyte resident index
                        // survives through ChainStage even if the cache is
                        // cleared.  Seed lookup has completed its Vulkan fence
                        // and copied anchors into `anchors_slab`, so neither
                        // index nor query-device storage is used downstream.
                        resident_index.reset();
                        device_span.reset();
                        if (post_seed_owned_lifetime != nullptr) {
                            const auto release =
                                post_seed_owned_lifetime->complete_seed_user();
                            proof.post_seed_owned_gpu_cache_evictions =
                                release.gpu_cache_evictions;
                            proof.post_seed_owned_gpu_resident_bytes_released =
                                release.gpu_resident_bytes_released;
                            proof.post_seed_owned_cpu_index_releases =
                                release.cpu_index_releases;
                            proof.post_seed_owned_cpu_position_bytes_released =
                                release.cpu_position_bytes_released;
                        }
                        proof_dst->record_partial_vulkan_execution_proof(proof);
                        return std::move(token).emit_batch(
                            std::span<Anchor>{anchors_slab.data(), anchors_slab.size()});
                    }
                    seed_gpu_declined = true;
                } else {
                    seed_decline.reason = bioseq::gpu::vulkan::
                        VulkanSeedLookupDeclineReason::hardware_device_unavailable;
                    seed_gpu_declined = true;
                }
            } else {
                seed_decline.reason = bioseq::gpu::vulkan::
                    VulkanSeedLookupDeclineReason::resident_index_unavailable;
                seed_gpu_declined = true;
            }
            if (seed_gpu_declined) {
                if (!seed_decline.declined()) {
                    seed_decline.reason = bioseq::gpu::vulkan::
                        VulkanSeedLookupDeclineReason::resident_index_unavailable;
                }
                bioseq::gpu::vulkan::throw_vulkan_seed_lookup_strict_decline(
                    "Vulkan seed lookup", seed_decline);
            }
        }

        if (direct_ledger != nullptr) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "source-direct resident route cannot fall back to CPU seeding"};
        }

        MatchResultCollection* const cpu_seed_proof_dst = proof_dst;
        PostSeedOwnedLifetime* const cpu_seed_owned_lifetime =
            post_seed_owned_lifetime;
        MatchResultCollection* const cpu_seed_host_timing_dst =
            host_timing_dst;
        const auto record_cpu_seed_completion =
            [cpu_seed_proof_dst, sketch_span, cpu_seed_owned_lifetime,
             cpu_seed_host_timing_dst, host_stage_started] {
            // A successful strict Vulkan lookup returned above, so this proof
            // can only describe a real CPU seed operation.
            if (cpu_seed_proof_dst == nullptr) {
                bioseq_log<ERR>("CPU seed stage has no execution-proof sink");
                std::abort();
            }
            ScalarPipelineExecutionProof proof{};
            proof.record_seed_lookup(
                static_cast<std::uint64_t>(sketch_span.size()));
            cpu_seed_proof_dst->record_scalar_pipeline_execution_proof(proof);

            if (cpu_seed_owned_lifetime != nullptr) {
                // CPU-index retirement is lifetime evidence, not Vulkan
                // execution evidence. Prepared fragment batches are normally
                // externally owned and leave this pointer null.
                (void)cpu_seed_owned_lifetime->complete_seed_user();
            }
            if (cpu_seed_host_timing_dst != nullptr) {
                cpu_seed_host_timing_dst->record_seed_lookup_host_time(
                    elapsed_host_stage_ns(host_stage_started));
            }
        };

        if constexpr (bioseq::indexing::PreparedFragmentAnchorQueryIndex<
                          std::remove_cvref_t<IndexT>>) {
            static_assert(std::same_as<MinimizerT, MinimapSketchData>,
                "prepared fragment anchors require canonical query minimizers");
            const std::span<const Anchor> ordered_anchors =
                target_index_ref.ordered_fragment_seed_anchors(
                    std::span<const MinimapSketchData>{
                        sketch_span.data(), sketch_span.size()},
                    query_len,
                    params.max_occ,
                    kmer_size);
            record_cpu_seed_completion();
            // The owning PreparedFragmentAnchorBatch outlives this synchronous
            // pipeline call. Emit its already-canonical span directly: no
            // ArenaSlab allocation, anchor copy, or global MPF sort.
            return std::move(token).emit_batch(ordered_anchors);
        } else {
            // Ordinary per-hash lookup retains the arena-producing kernel.
            auto anchors_slab = make_arena_slab<Anchor>();
            anchors_slab.reserve(detail::estimated_anchor_count(
                sketch_span.size(),
                static_cast<std::size_t>(query_len),
                static_cast<std::size_t>(target_index_ref.target_length()),
                kmer_size));
            collect_seed_hits<MinimizerT>(
                sketch_span,
                target_index_ref,
                query_len, params.max_occ, kmer_size,
                anchors_slab);
            record_cpu_seed_completion();
            return std::move(token).emit_batch(
                std::span<Anchor>{anchors_slab.data(), anchors_slab.size()});
        }
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = "Seed hits + canonical CPU ordering";
        algo.parameters.push_back({"max_occ", std::to_string(params.max_occ)});
        algo.parameters.push_back({"query_len", std::to_string(query_len)});
        desc.algorithm = std::move(algo);
        // Anchor reserve target — used by SeedHitStage's slab (DOC0249 §21.4
        // "estimated_anchor_count").  The actual sketch count is unknown at
        // describe time so the reported figure uses query_len/window=10 as
        // the canonical median; the runtime reserve picks a tighter value
        // when the real sketches.size() is available.
        const std::size_t approx_sketch_count =
            static_cast<std::size_t>(query_len) / 10u + 64u;
        desc.estimated_batch_size = detail::estimated_anchor_count(
            approx_sketch_count,
            static_cast<std::size_t>(query_len),
            /*target_bp=*/static_cast<std::size_t>(query_len),
            params.kmer_size);
        return desc;
    }
};

/**
 * @brief Stage 3: anchor chaining.
 *
 * Templated on `Kernel` — the algorithm-specific call surface exposed by
 * `algorithm_traits<ChainingAlgorithmType::X>::kernel_type` (DOC0249 §8.0,
 * R5 trait pattern).  The stage body has zero `if constexpr` over enum
 * values — adding a new chaining algorithm is a single trait specialisation
 * in the kernel's own module.
 *
 * The chosen kernel writes its `(chains, reordered_anchors)` output directly
 * into caller-provided ArenaSlab parameters (DOC0249 §13 follow-up:
 * output-side kernel-to-arena refactor); no intermediate vector is
 * materialised inside the stage.
 */
template <class Kernel>
struct ChainStage : StageBase<ChainStage<Kernel>, Chain> {
    static constexpr std::string_view stage_name = "ChainStage";

    ChainingParams cp;
    sequence_size_t query_len = 0;
    MatchResultCollection* proof_dst = nullptr;
    MatchResultCollection* host_timing_dst = nullptr;

    template <is_constexpr_aligner_params Cep, class B, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process(B&& anchors_in, EmitToken<Sink>&& token) {
        const std::uint64_t host_stage_started = host_timing_dst == nullptr
            ? 0u : host_stage_now_ns();
        ChainingParams active_cp = cp;
        active_cp.query_len = query_len;
        const std::span<const Anchor> anchors_span{
            anchors_in.data(), anchors_in.size()};

        auto chains_slab    = make_arena_slab<Chain>();
        auto reordered_slab = make_arena_slab<Anchor>();
        if (active_cp.coordinate_only) {
            chains_slab.reserve(1);
            // Reserve the full upstream bound before the Vulkan backend opens
            // its nested DP arena scope. A >65,536-anchor coordinate trace
            // must never grow this output slab inside that inner scope, whose
            // allocations are reclaimed before the stage emits downstream.
            reordered_slab.reserve(anchors_in.size());
        } else {
            chains_slab.reserve(detail::estimated_chain_count(
                anchors_in.size(), active_cp.min_seed_count));
            reordered_slab.reserve(anchors_in.size());
        }

        if constexpr (Kernel::emits_scalar_chaining_execution_proof) {
            if (proof_dst == nullptr) {
                bioseq_log<ERR>(
                    "Scalar chaining stage has no execution-proof sink");
                std::abort();
            }
            ScalarPipelineExecutionProof proof{};
            Kernel::call(
                anchors_span, active_cp, chains_slab, reordered_slab, &proof);
            proof_dst->record_scalar_pipeline_execution_proof(proof);
        } else if constexpr (Kernel::emits_simd_chaining_execution_proof) {
            if (proof_dst == nullptr) {
                bioseq_log<ERR>("SIMD chaining stage has no execution-proof sink");
                std::abort();
            }
            SimdChainingExecutionProof proof{};
            Kernel::call(
                anchors_span, active_cp, chains_slab, reordered_slab, &proof);
            if (!proof.accepted(
                    active_cp.simd_policy,
                    /*require_nonempty_vector_work=*/false)) {
                const auto status = proof.policy_evidence_coherent(
                        active_cp.simd_policy)
                    ? SimdChainingStatus::invalid_parameters
                    : SimdChainingStatus::invalid_policy;
                throw SimdChainingRuntimeError(
                    status,
                    "selected policy does not match operation-originated "
                    "SIMD chaining proof");
            }
            proof_dst->record_simd_chaining_execution_proof(proof);
        } else if constexpr (Kernel::emits_partial_vulkan_execution_proof) {
            if (proof_dst == nullptr) {
                bioseq_log<ERR>(
                    "Vulkan chaining stage has no execution-proof sink");
                std::abort();
            }
            if (Kernel::call(
                    anchors_span, active_cp, chains_slab, reordered_slab)) {
                PartialVulkanExecutionProof proof{};
                proof.chain_score_dispatches = 1;
                proof_dst->record_partial_vulkan_execution_proof(proof);
            }
        } else {
            Kernel::call(anchors_span, active_cp, chains_slab, reordered_slab);
        }

        if (host_timing_dst != nullptr) {
            host_timing_dst->record_chain_score_host_time(
                elapsed_host_stage_ns(host_stage_started));
        }

        return std::move(token).emit_batch_pair(
            std::span<Chain> {chains_slab.data(),    chains_slab.size()},
            std::span<Anchor>{reordered_slab.data(), reordered_slab.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = std::string{Kernel::algorithm_label()};
        algo.parameters.push_back({"min_chain_score", std::to_string(cp.min_chain_score)});
        algo.parameters.push_back({"min_seed_count",  std::to_string(cp.min_seed_count)});
        algo.parameters.push_back({"max_chain_skip",  std::to_string(cp.max_chain_skip)});
        algo.parameters.push_back({"max_chain_iter",  std::to_string(cp.max_chain_iter)});
        algo.parameters.push_back({"bandwidth",       std::to_string(cp.bandwidth)});
        algo.parameters.push_back({"max_gap",         std::to_string(cp.max_gap)});
        // Let the selected kernel append its own runtime params (e.g. the
        // Vulkan backend's phase / vulkan_enabled markers) so the
        // EXPLAIN metadata is not dead code. No-op for the scalar/SIMD/RMQ
        // kernels (DOC0304 §7.2).
        Kernel::populate_runtime_params(cp, algo.parameters);
        algo.computed_properties = Kernel::template static_properties<Cep>();
        desc.algorithm = std::move(algo);
        // Chain stage's two arena slabs are sized from the runtime
        // anchor count (which is unknown at describe time), so we leave
        // `estimated_batch_size` empty rather than fabricate a per-query
        // figure that doesn't reflect the actual reserve.  The
        // `[reserved: ...]` line in EXPLAIN is therefore omitted for
        // ChainStage; the runtime `chains_slab.reserve(...)` call still
        // sizes from `estimated_chain_count(anchors_in.size(), ...)`.
        return desc;
    }
};

/**
 * @brief Stage 4: chain → region conversion.
 *
 * Wraps the `generate_regions` kernel, which writes its
 * `AlignRegion`-per-chain output directly into a caller-provided
 * ArenaSlab via placement-new (DOC0249 §13 follow-up: output-side
 * kernel-to-arena refactor).  `AlignRegion` owns a `std::vector<CigarOp>`,
 * so it is not trivially-destructible; the slab's destructor walks
 * `[0, len_)` and calls `~AlignRegion()` on each live element before the
 * arena reclaims the storage.
 */
struct RegionStage : StageBase<RegionStage, AlignRegion> {
    static constexpr std::string_view stage_name = "RegionStage";

    sequence_size_t query_len = 0;
    MatchResultCollection* host_timing_dst = nullptr;

    template <is_constexpr_aligner_params Cep, class CB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(CB&& chains, AB&& anchors, EmitToken<Sink>&& token) {
        if (host_timing_dst != nullptr) {
            host_timing_dst->begin_region_host_phase(host_stage_now_ns());
        }
        // `AlignRegion` owns a `std::vector<CigarOp>` so the slab uses the
        // placement-new pattern; no intermediate `std::vector` is needed.
        auto regions_slab = make_arena_slab<AlignRegion>();
        generate_regions(
            std::span<Chain>{chains.data(), chains.size()},
            std::span<const Anchor>{anchors.data(), anchors.size()},
            query_len,
            regions_slab);

        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions_slab.data(), regions_slab.size()},
            std::span<const Anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = "Region generation";
        algo.parameters.push_back({"query_len", std::to_string(query_len)});
        desc.algorithm = std::move(algo);
        // One region per chain (exact); the actual chain count is only
        // known at runtime so the describe-time figure is left empty.  The
        // runtime reserve in process_pair sizes the slab from chains.size().
        return desc;
    }
};

/**
 * @brief Stage 5: classify primary/secondary in place.
 *
 * Mutates the regions span via `classify_primary_secondary` (which takes
 * std::vector<AlignRegion>& and modifies parent / score fields).  The
 * vector-wrapper here copies in and out; the touched region count is bounded
 * by best_n + 16 so the cost is negligible.
 */
struct PrimarySecondaryStage : StageBase<PrimarySecondaryStage, AlignRegion> {
    static constexpr std::string_view stage_name = "PrimarySecondaryStage";

    float   mask_level;
    int32_t mask_len;
    int32_t sub_diff_value;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& anchors, EmitToken<Sink>&& token) {
        // Kernel now mutates in place through a span — no vector round-trip
        // (DOC0249 M8).
        classify_primary_secondary(
            std::span<AlignRegion>{regions.data(), regions.size()},
            mask_level, mask_len, sub_diff_value);
        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions.data(), regions.size()},
            std::span<const Anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = "Classify primary/secondary";
        algo.parameters.push_back({"mask_level",
            std::to_string(static_cast<double>(mask_level))});
        algo.parameters.push_back({"mask_len", std::to_string(mask_len)});
        algo.parameters.push_back({"sub_diff", std::to_string(sub_diff_value)});
        desc.algorithm = std::move(algo);
        return desc;
    }
};

/**
 * @brief Stage 6: filter secondary regions, shrinking the span in place.
 */
struct SecondaryFilterStage : StageBase<SecondaryFilterStage, AlignRegion> {
    static constexpr std::string_view stage_name = "SecondaryFilterStage";

    float   pri_ratio;
    int32_t min_diff_value;
    int32_t best_n;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& anchors, EmitToken<Sink>&& token) {
        // Kernel filters in place via a span; the new effective size is
        // returned so the downstream stage observes a shorter prefix
        // (DOC0249 M8 — `filter_secondary_regions_span`).
        const std::size_t new_size = filter_secondary_regions(
            std::span<AlignRegion>{regions.data(), regions.size()},
            pri_ratio, min_diff_value, best_n);
        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions.data(), new_size},
            std::span<const Anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = "Filter secondary regions";
        algo.parameters.push_back({"pri_ratio",
            std::to_string(static_cast<double>(pri_ratio))});
        algo.parameters.push_back({"min_diff", std::to_string(min_diff_value)});
        algo.parameters.push_back({"best_n",   std::to_string(best_n)});
        desc.algorithm = std::move(algo);
        return desc;
    }
};

/**
 * @brief Stage 7: estimate divergence (conditional).
 */
template <class MinimizerT, class QueryMinimizers = std::vector<MinimizerT>>
struct DivergenceStage : StageBase<DivergenceStage<MinimizerT, QueryMinimizers>, AlignRegion> {
    static constexpr std::string_view stage_name = "DivergenceStage";

    const QueryMinimizers* query_minimizers_;
    sequence_size_t query_len;
    sequence_size_t target_len;
    int32_t runtime_kmer_size;
    bool    enabled;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& anchors, EmitToken<Sink>&& token) {
        if (enabled) {
            const int32_t kmer_size = Cep::generic_fallback
                ? runtime_kmer_size
                : static_cast<int32_t>(Cep::kmer_len);
            estimate_divergence_from_seeds<MinimizerT>(
                std::span<AlignRegion>{regions.data(), regions.size()},
                std::span<const MinimizerT>{
                    query_minimizers_->data(), query_minimizers_->size()},
                std::span<const Anchor>{anchors.data(), anchors.size()},
                query_len, kmer_size, target_len);
        }
        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions.data(), regions.size()},
            std::span<const Anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        if (enabled) {
            StageAlgorithmDescription algo;
            algo.name = "Estimate divergence from seeds";
            desc.algorithm = std::move(algo);
        }
        return desc;
    }
};

[[nodiscard]] inline ::bioseq::prefilter_stats::ResolvedPrefilterAlgorithm
resolve_prefilter_algorithm(
    ::bioseq::prefilter_stats::PrefilterBackendType backend,
    std::span<const prefilters::PrefilterJob> jobs) noexcept
{
    using Backend = ::bioseq::prefilter_stats::PrefilterBackendType;
    using Algorithm = ::bioseq::prefilter_stats::ResolvedPrefilterAlgorithm;
    if (backend != Backend::VULKAN) {
        return Algorithm::sneaky_snake;
    }
    for (const prefilters::PrefilterJob& job : jobs) {
        const bool measured_length = job.query_length == job.target_length
            && (job.query_length == 32u || job.query_length == 64u);
        const bool measured_threshold =
            job.max_edits == 2u || job.max_edits == 4u;
        if (!measured_length || !measured_threshold) {
            return Algorithm::sneaky_snake;
        }
    }
    return jobs.empty()
        ? Algorithm::sneaky_snake
        : Algorithm::gatekeeper;
}

/**
 * Query-scoped rank-prefilter lower-bound gate.  Only mandatory global
 * seed-to-seed fills are evaluated; local left/right extension windows are
 * intentionally excluded.  A region is removed only when the sum of proven
 * per-fill lower bounds exceeds the SQL predicate's maximum edit count.
 */
template <sequence_alphabet Alphabet>
struct PrefilterStage
    : StageBase<PrefilterStage<Alphabet>, AlignRegion> {
    static constexpr std::string_view stage_name = "PrefilterStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq_;
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq_;
    MatchResultCollection* proof_dst_ = nullptr;

    [[nodiscard]] static constexpr std::string_view backend_name(
        ::bioseq::prefilter_stats::PrefilterBackendType backend) noexcept {
        using Backend = ::bioseq::prefilter_stats::PrefilterBackendType;
        switch (backend) {
            case Backend::AUTO: return "auto";
            case Backend::SCALAR: return "scalar";
            case Backend::SIMD: return "highway_simd";
            case Backend::VULKAN: return "vulkan";
        }
        return "unknown";
    }

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    EmittedToken<Sink> process_pair(
        RB&& regions, AB&& anchors, EmitToken<Sink>&& token) {
        if (!params.prefilter_enabled || !params.compute_edit_distance
            || regions.size() == 0) {
            return std::move(token).emit_batch_pair(
                std::span<AlignRegion>{regions.data(), regions.size()},
                std::span<const Anchor>{anchors.data(), anchors.size()});
        }

        static const auto* prefilter_region =
            bioseq::instr::register_region_static("prefilter");
        auto prefilter_scope = bioseq::instr::region(prefilter_region);

        std::vector<std::uint8_t> query_ranks;
        std::vector<std::uint8_t> target_ranks;
        std::vector<prefilters::PrefilterJob> jobs;
        std::vector<std::size_t> job_regions;
        const std::uint64_t max_flat_offset =
            (std::numeric_limits<std::uint32_t>::max)();

        for (std::size_t region_index = 0;
             region_index < regions.size(); ++region_index) {
            const AlignRegion& region = regions[region_index];
            (void)for_each_mandatory_alignment_gap(
                region,
                std::span<const Anchor>{anchors.data(), anchors.size()},
                params,
                [this, &query_ranks, &target_ranks, &jobs, &job_regions,
                 &region, region_index, max_flat_offset](
                    const MandatoryAlignmentGapWindow& window) {
                    const std::uint64_t query_length =
                        window.query_end - window.query_start;
                    const std::uint64_t target_length =
                        window.target_end - window.target_start;
                    if (window.query_end > query_seq_.bp_size()
                        || window.target_end > target_seq_.bp_size()
                        || query_length > max_flat_offset
                        || target_length > max_flat_offset
                        || query_ranks.size() > max_flat_offset - query_length
                        || target_ranks.size() > max_flat_offset - target_length) {
                        /* Unsupported valid geometry is a conservative accept,
                         * never a hidden fallback or false rejection. */
                        return true;
                    }

                    const auto query_offset = static_cast<std::uint32_t>(
                        query_ranks.size());
                    const auto target_offset = static_cast<std::uint32_t>(
                        target_ranks.size());
                    query_ranks.resize(
                        query_ranks.size() + static_cast<std::size_t>(query_length));
                    target_ranks.resize(
                        target_ranks.size() + static_cast<std::size_t>(target_length));
                    std::span<std::uint8_t> query_destination{
                        query_ranks.data() + query_offset,
                        static_cast<std::size_t>(query_length)};
                    std::span<std::uint8_t> target_destination{
                        target_ranks.data() + target_offset,
                        static_cast<std::size_t>(target_length)};
                    if (region.is_reverse) {
                        query_seq_.template to_encoded_into<true, false>(
                            query_destination,
                            static_cast<std::size_t>(window.query_start),
                            static_cast<std::size_t>(window.query_end));
                    } else {
                        query_seq_.template to_encoded_into<false, false>(
                            query_destination,
                            static_cast<std::size_t>(window.query_start),
                            static_cast<std::size_t>(window.query_end));
                    }
                    target_seq_.template to_encoded_into<false, false>(
                        target_destination,
                        static_cast<std::size_t>(window.target_start),
                        static_cast<std::size_t>(window.target_end));
                    jobs.push_back(prefilters::PrefilterJob{
                        .query_offset = query_offset,
                        .target_offset = target_offset,
                        .query_length = static_cast<std::uint32_t>(query_length),
                        .target_length = static_cast<std::uint32_t>(target_length),
                        .max_edits = params.prefilter_max_edits,
                    });
                    job_regions.push_back(region_index);
                    return true;
                });
        }

        if (jobs.empty()) {
            return std::move(token).emit_batch_pair(
                std::span<AlignRegion>{regions.data(), regions.size()},
                std::span<const Anchor>{anchors.data(), anchors.size()});
        }

        const prefilters::PrefilterBatchInput input{
            .query_ranks = query_ranks,
            .target_ranks = target_ranks,
            .jobs = jobs,
        };
        std::vector<prefilters::PrefilterResult> results;
        using Backend = ::bioseq::prefilter_stats::PrefilterBackendType;
        using Algorithm = ::bioseq::prefilter_stats::ResolvedPrefilterAlgorithm;
        const Backend selected = params.prefilter_backend
            == Backend::AUTO
                ? Backend::SIMD
                : params.prefilter_backend;
        const Algorithm algorithm =
            resolve_prefilter_algorithm(selected, jobs);
        switch (selected) {
            case Backend::SCALAR:
                results = sneaky_snake_scalar_batch(input);
                break;
            case Backend::SIMD:
            case Backend::AUTO:
                results = sneaky_snake_simd_batch(input);
                break;
            case Backend::VULKAN: {
                if (algorithm == Algorithm::gatekeeper) {
                    results = prefilters::run_gatekeeper_vulkan_batch(
                        query_ranks, target_ranks, jobs);
                } else {
                    results = prefilters::run_sneaky_snake_vulkan_batch(
                        query_ranks, target_ranks, jobs);
                }
                break;
            }
        }
        if (results.size() != jobs.size()) {
            throw std::runtime_error(
                "rank prefilter backend returned the wrong result count");
        }

        const std::size_t input_region_count = regions.size();
        std::uint64_t evaluated_jobs = 0u;
        std::uint64_t length_64_jobs = 0u;
        std::uint64_t length_difference_jobs = 0u;
        std::uint64_t unsupported_jobs = 0u;
        std::uint64_t rejected_jobs = 0u;
        std::uint64_t mask_rejection_candidates = 0u;
        std::uint64_t exact_guard_activations = 0u;
        std::uint64_t guard_confirmed_rejections = 0u;
        std::uint64_t hamming_exact_rejections = 0u;
        std::vector<std::uint64_t> lower_bounds(regions.size(), 0u);
        std::vector<bool> keep(regions.size(), true);
        for (std::size_t index = 0; index < results.size(); ++index) {
            const prefilters::PrefilterResult& result = results[index];
            const std::size_t region_index = job_regions[index];
            switch (static_cast<prefilters::PrefilterStatus>(result.status)) {
                case prefilters::PrefilterStatus::success:
                    ++evaluated_jobs;
                    if (jobs[index].query_length == 64u
                        && jobs[index].target_length == 64u) {
                        ++length_64_jobs;
                    }
                    break;
                case prefilters::PrefilterStatus::conservative_unequal:
                    ++length_difference_jobs;
                    break;
                case prefilters::PrefilterStatus::unsupported_conservative:
                    ++unsupported_jobs;
                    break;
                default:
                    throw std::runtime_error(
                        "rank prefilter backend returned an unknown status");
            }
            if (algorithm == Algorithm::gatekeeper) {
                mask_rejection_candidates +=
                    prefilters::gatekeeper_mask_rejection_candidate(result)
                        ? 1u : 0u;
                exact_guard_activations +=
                    prefilters::gatekeeper_exact_guard_activated(result)
                        ? 1u : 0u;
                guard_confirmed_rejections +=
                    prefilters::gatekeeper_guard_confirmed_rejection(result)
                        ? 1u : 0u;
                hamming_exact_rejections +=
                    prefilters::gatekeeper_hamming_exact_rejection(result)
                        ? 1u : 0u;
            }
            if (!prefilters::prefilter_accepted(result)) ++rejected_jobs;
            if (result.status == static_cast<std::uint32_t>(
                    prefilters::PrefilterStatus::unsupported_conservative)) continue;
            lower_bounds[region_index] =
                bioseq::parallelism::saturating_add(
                    lower_bounds[region_index],
                    static_cast<std::uint64_t>(result.lower_bound));
            if (!prefilters::prefilter_accepted(result)
                || lower_bounds[region_index] > params.prefilter_max_edits) {
                keep[region_index] = false;
            }
        }

        std::size_t output_size = 0;
        for (std::size_t index = 0; index < regions.size(); ++index) {
            if (!keep[index]) continue;
            if (output_size != index) {
                regions.data()[output_size] = std::move(regions.data()[index]);
            }
            ++output_size;
        }
        if (proof_dst_ == nullptr) {
            throw std::logic_error(
                "rank prefilter stage has no execution-proof sink");
        }
        ::bioseq::prefilter_stats::PrefilterExecutionProof call_proof{};
        call_proof.record_batch(
            algorithm,
            selected,
            static_cast<std::uint64_t>(jobs.size()),
            evaluated_jobs,
            length_difference_jobs,
            unsupported_jobs,
            rejected_jobs,
            static_cast<std::uint64_t>(input_region_count - output_size),
            static_cast<std::uint64_t>(output_size),
            mask_rejection_candidates,
            exact_guard_activations,
            guard_confirmed_rejections,
            hamming_exact_rejections,
            length_64_jobs);
        proof_dst_->record_prefilter_execution_proof(call_proof);
        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions.data(), output_size},
            std::span<const Anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        using Backend = ::bioseq::prefilter_stats::PrefilterBackendType;
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        if (params.prefilter_enabled) {
            StageAlgorithmDescription algorithm;
            algorithm.name = "Rank edit-distance prefilter";
            algorithm.parameters.push_back({
                "backend", std::string{backend_name(params.prefilter_backend)}});
            algorithm.parameters.push_back({
                "algorithm",
                params.prefilter_backend == Backend::VULKAN
                    ? "batch_resolved_gatekeeper_or_sneaky_snake"
                    : "sneaky_snake"});
            algorithm.parameters.push_back({
                "max_edits", std::to_string(params.prefilter_max_edits)});
            algorithm.parameters.push_back({
                "submaze_width",
                std::to_string(sneaky_snake_default_submaze_width)});
            algorithm.computed_properties.push_back({
                "domain", "mandatory seed-to-seed global fills"});
            algorithm.computed_properties.push_back({
                "survivor_check", "exact post-alignment edit distance"});
            algorithm.computed_properties.push_back({
                "gatekeeper_auto_domain", "length in {32,64}, E in {2,4}"});
            desc.algorithm = std::move(algorithm);
        }
        return desc;
    }
};

/**
 * Authoritative exact edit-distance predicate gate.
 *
 * This runs after traceback has populated `AlignRegion::edit_distance` and
 * before the final primary/secondary and MAPQ pass.  Consequently a candidate
 * that does not satisfy the SQL predicate cannot influence the classification
 * or mapping-quality metadata of a candidate that does.  The earlier
 * rank-prefilter stage remains only a conservative work-avoidance gate.
 */
struct ExactEditFilterStage
    : StageBase<ExactEditFilterStage, AlignRegion> {
    static constexpr std::string_view stage_name = "ExactEditFilterStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    EmittedToken<Sink> process_pair(
        RB&& regions, AB&& anchors, EmitToken<Sink>&& token) {
        if (!params.exact_edit_filter_enabled) {
            return std::move(token).emit_batch_pair(
                std::span<AlignRegion>{regions.data(), regions.size()},
                std::span<const Anchor>{anchors.data(), anchors.size()});
        }
        if (!params.compute_edit_distance) {
            throw std::logic_error(
                "exact edit filter requires exact edit-distance accounting");
        }

        const std::int64_t exclusive_upper_bound =
            params.exact_edit_filter_exclusive_upper_bound;
        std::size_t output_size = 0;
        for (std::size_t index = 0; index < regions.size(); ++index) {
            AlignRegion& region = regions.data()[index];
            if (!region.has_alignment) {
                throw std::runtime_error(
                    "exact edit filter received a region without alignment traceback");
            }
            const bool keep = exclusive_upper_bound > 0
                && region.edit_distance < static_cast<std::uint64_t>(
                    exclusive_upper_bound);
            if (!keep) continue;

            if (output_size != index) {
                regions.data()[output_size] = std::move(region);
            }
            AlignRegion& survivor = regions.data()[output_size];
            survivor.region_id = static_cast<std::int32_t>(output_size);
            survivor.parent = -1;
            survivor.secondary_score = 0;
            survivor.secondary_count = 0;
            survivor.secondary_dp_max = 0;
            survivor.mapq = 0;
            ++output_size;
        }

        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions.data(), output_size},
            std::span<const Anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        if (params.exact_edit_filter_enabled) {
            StageAlgorithmDescription algorithm;
            algorithm.name = "Exact edit-distance predicate";
            algorithm.parameters.push_back({
                "exclusive_upper_bound",
                std::to_string(
                    params.exact_edit_filter_exclusive_upper_bound)});
            algorithm.computed_properties.push_back({
                "classification_domain", "exact predicate survivors"});
            desc.algorithm = std::move(algorithm);
        }
        return desc;
    }
};

/**
 * @brief Stage 8: base-level alignment via the chosen aligner (conditional).
 *
 * DOC0254 §6.3 — reparameterised on `Kernel = kernel_t<BAA>`.  Algorithm
 * dispatch happens exclusively via the trait; the `if constexpr (BAA == ...)`
 * ladder inside `describe()` is gone — its rows now live in
 * `Kernel::populate_runtime_params(...)`.
 */
template <class Kernel,
          sequence_alphabet Alphabet>
struct AlignStage : StageBase<AlignStage<Kernel, Alphabet>, AlignRegion> {
    static constexpr std::string_view stage_name = "AlignStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    const ScoringMatrix<Alphabet>*                sm;
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq_;
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq_;
    bool                                          enabled;
    MatchResultCollection*                        proof_dst_;
    MatchResultCollection*                        host_timing_dst_;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& anchors, EmitToken<Sink>&& token) {
        if (host_timing_dst_ != nullptr) {
            host_timing_dst_->transition_region_to_alignment_result_host_phase(
                host_stage_now_ns(), enabled);
        }
        if (enabled) {
            if constexpr (Kernel::emits_scalar_ksw2_execution_proof) {
                if (proof_dst_ == nullptr) {
                    bioseq_log<ERR>(
                        "Scalar KSW2 stage has no execution-proof sink");
                    std::abort();
                }
                ScalarPipelineExecutionProof call_proof{};
                Kernel::template call<Alphabet>(
                    std::span<AlignRegion>{regions.data(), regions.size()},
                    std::span<const ::bioseq::chaining::Anchor>{
                        anchors.data(), anchors.size()},
                    query_seq_, target_seq_, *sm, params,
                    /*forbidden_block_proof=*/nullptr,
                    &call_proof);
                proof_dst_->record_scalar_pipeline_execution_proof(
                    call_proof);
            } else if constexpr (
                    Kernel::emits_ksw2_batched_execution_proof) {
                if (proof_dst_ == nullptr) {
                    bioseq_log<ERR>(
                        "KSW2 batched stage has no execution-proof sink");
                    std::abort();
                }
                Ksw2BatchedExecutionProof call_proof{};
                Kernel::template call<Alphabet>(
                    std::span<AlignRegion>{regions.data(), regions.size()},
                    std::span<const ::bioseq::chaining::Anchor>{
                        anchors.data(), anchors.size()},
                    query_seq_, target_seq_, *sm, params,
                    /*forbidden_block_proof=*/nullptr,
                    /*scalar_execution_proof=*/nullptr,
                    &call_proof);
                if (!call_proof.accepted(
                        /*require_nonempty_work=*/false)) {
                    bioseq_log<ERR>(
                        "KSW2 batched stage produced rejected execution proof");
                    std::abort();
                }
                proof_dst_->record_ksw2_batched_execution_proof(
                    call_proof);
            } else if constexpr (Kernel::emits_block_execution_proof) {
                if (proof_dst_ == nullptr) {
                    bioseq_log<ERR>(
                        "Block Aligner stage has no execution-proof sink");
                    std::abort();
                }
                BlockAlignerExecutionProof call_proof{};
                Kernel::template call<Alphabet>(
                    std::span<AlignRegion>{regions.data(), regions.size()},
                    std::span<const ::bioseq::chaining::Anchor>{
                        anchors.data(), anchors.size()},
                    query_seq_, target_seq_, *sm, params, &call_proof,
                    /*scalar_execution_proof=*/nullptr);
                if (!call_proof.accepted()) {
                    bioseq_log<ERR>(
                        "Block Aligner stage produced a rejected execution proof");
                    std::abort();
                }
                proof_dst_->record_block_aligner_execution_proof(
                    call_proof);
            } else {
                Kernel::template call<Alphabet>(
                    std::span<AlignRegion>{regions.data(), regions.size()},
                    std::span<const ::bioseq::chaining::Anchor>{
                        anchors.data(), anchors.size()},
                    query_seq_, target_seq_, *sm, params,
                    /*forbidden_block_proof=*/nullptr,
                    /*scalar_execution_proof=*/nullptr);
            }
        }
        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions.data(), regions.size()},
            std::span<const ::bioseq::chaining::Anchor>{
                anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        if (enabled) {
            StageAlgorithmDescription algo;
            algo.name = std::string{Kernel::algorithm_label()};
            Kernel::populate_runtime_params(params, algo.parameters);
            algo.computed_properties = Kernel::template static_properties<Cep>();
            desc.algorithm = std::move(algo);
        }
        return desc;
    }
};

/**
 * @brief Stage 9: compute MAPQ (conditional).
 *
 * Accepts a `(regions, anchors)` pair upstream but only uses the regions
 * span; emits the regions span downstream as a single batch (DOC0249 §8.10).
 */
struct MapqStage : StageBase<MapqStage, AlignRegion> {
    static constexpr std::string_view stage_name = "MapqStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    bool                                          enabled;

    template <is_constexpr_aligner_params Cep, class RB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process(RB&& regions, EmitToken<Sink>&& token) {
        if (enabled) {
            compute_mapping_quality(
                std::span<AlignRegion>{regions.data(), regions.size()},
                params, 0);
        }
        return std::move(token).emit_batch(
            std::span<AlignRegion>{regions.data(), regions.size()});
    }

    // DOC0254 §4.3: pair → single delegation.  The outer body has no
    // surviving `EmitToken<Sink>` after the inner call moves it, so a
    // second emit is structurally blocked.
    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& /*unused*/, EmitToken<Sink>&& token) {
        return process<Cep>(std::forward<RB>(regions), std::move(token));
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        if (enabled) {
            StageAlgorithmDescription algo;
            algo.name = "Compute mapping quality (mm_set_mapq2)";
            desc.algorithm = std::move(algo);
        }
        return desc;
    }
};

/**
 * @brief Terminal sink: push converted MatchResults into the collection.
 *
 * Not a `BareStage` — appears as the tail of `make_pipeline<Cep>(...)`.
 */
struct SinkStage {
    MatchResultCollection* dst_;
    sequence_size_t        query_len  = 0;
    sequence_size_t        target_len = 0;
    MatchResultCollection* host_timing_dst_ = nullptr;

    [[gnu::always_inline]] inline void on_batch(std::span<AlignRegion> regions) {
        dst_->results.reserve(dst_->results.size() + regions.size());
        // `region_slot` is move-consumed into the converted MatchResult; the
        // slot is left in a valid-but-moved-from state and reclaimed by
        // whichever owner (arena slab or std::vector) holds the span.
        for (auto& region_slot : regions) {
            dst_->results.push_back(
                region_to_match_result(std::move(region_slot), query_len, target_len));
        }
        if (host_timing_dst_ != nullptr
            && host_timing_dst_->alignment_result_host_phase_active) {
            host_timing_dst_->finish_alignment_result_host_phase(
                host_stage_now_ns());
        }
    }

    [[gnu::always_inline]] inline void on_completed() noexcept {
        if (host_timing_dst_ != nullptr
            && host_timing_dst_->alignment_result_host_phase_active) {
            host_timing_dst_->finish_alignment_result_host_phase(
                host_stage_now_ns());
        }
    }
};

// =============================================================================
// 11.2 describe() walker — out-of-line definition (Stage forward-declared above)
// =============================================================================

template <is_constexpr_aligner_params Cep, class Self, class Sink>
[[nodiscard]] PipelineDescription Stage<Cep, Self, Sink>::describe() const {
    // DOC0254 §9.2 / R11: every concrete stage's `describe()` is templated on
    // Cep so describe-time Cep-sensitive output always matches the runtime
    // path's behaviour.  No `describe_representative_cep_t` alias.
    PipelineDescription d;
    d.stages.push_back(self_.template describe<Cep>());
    if constexpr (requires { sink_.describe(); }) {
        PipelineDescription rest = sink_.describe();
        d.stages.insert(d.stages.end(),
            std::make_move_iterator(rest.stages.begin()),
            std::make_move_iterator(rest.stages.end()));
    }
    return d;
}

/**
 * @brief Injects the Cep computed properties as a leading entry in the
 * pipeline description so the EXPLAIN renderer sees w/k/L beside the first
 * stage.
 */
template <is_constexpr_aligner_params Cep, class Self, class Sink>
[[nodiscard]] PipelineDescription describe_with_params(
    const Stage<Cep, Self, Sink>& pipeline)
{
    auto d = pipeline.describe();
    if (!d.stages.empty()) {
        auto& first = d.stages.front();
        if (!first.algorithm.has_value()) {
            first.algorithm.emplace();
            first.algorithm->name = "pipeline configuration";
        }
        first.algorithm->computed_properties.push_back(
            {"cep.window_len", std::to_string(Cep::window_len)});
        first.algorithm->computed_properties.push_back(
            {"cep.kmer_len",   std::to_string(Cep::kmer_len)});
        first.algorithm->computed_properties.push_back(
            {"cep.sequence_len_log2", std::to_string(Cep::sequence_len_log2)});
        first.algorithm->computed_properties.push_back(
            {"cep.specialised", Cep::generic_fallback ? "fallback" : "specialised"});
    }
    return d;
}

// =============================================================================
// 9.4 make_minimap_pipeline — single source of truth for the pipeline shape
// =============================================================================

[[nodiscard]] inline bool vulkan_components_requested(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept;

/**
 * @brief Build the full minimap pipeline value for one (Cep, BAA, CA, MA, EP).
 *
 * All template parameters are fixed by the caller (the dispatcher cascade).
 * The function is the single place both the run path and the describe path
 * consult to learn what stages are present — adding or removing a stage edits
 * only this function (DOC0249 §9.4).
 */
template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          sequence_alphabet Alphabet,
          class MinimizerT, class IndexT,
          class QueryMinimizers = std::vector<MinimizerT>>
[[nodiscard, gnu::always_inline]] inline auto make_minimap_pipeline(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const QueryMinimizers&            query_minimizers,
    const IndexT&                     target_index,
    const ScoringMatrix<Alphabet>&    scoring_matrix,
    MatchResultCollection&            output_collection,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    sequence_size_t                   query_len,
    sequence_size_t                   target_len,
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {},
    PostSeedOwnedLifetime* post_seed_owned_lifetime = nullptr)
{
    MatchResultCollection* host_timing_dst =
        vulkan_components_requested(params) ? nullptr : &output_collection;
    MatchResultCollection* chaining_proof_dst = nullptr;
    if constexpr (CA
            == ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SCALAR_MM2
        || CA
            == ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SIMD_MM2
        || CA
            == ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::VULKAN_TILED_DP) {
        chaining_proof_dst = &output_collection;
    }
    MatchResultCollection* alignment_proof_dst = nullptr;
    if constexpr (kernel_t<BAA>::emits_block_execution_proof
        || kernel_t<BAA>::emits_scalar_ksw2_execution_proof
        || kernel_t<BAA>::emits_ksw2_batched_execution_proof) {
        alignment_proof_dst = &output_collection;
    }
    auto tail = make_pipeline<Cep>(
        IndexStage<IndexT>{ {}, &target_index, gpu_index_identity },
        SeedHitStage{ {}, params, query_len, gpu_index_cache,
                      &output_collection, post_seed_owned_lifetime,
                      host_timing_dst },
        ChainStage<kernel_t<CA>>{
            {}, params.to_chaining_params(), query_len,
            chaining_proof_dst, host_timing_dst },
        RegionStage{ {}, query_len, host_timing_dst },
        PrimarySecondaryStage{ {}, params.mask_level, params.mask_len, sub_diff(params) },
        SecondaryFilterStage{ {}, params.pri_ratio, min_diff(params), params.best_n },
        DivergenceStage<MinimizerT, QueryMinimizers>{
            {}, &query_minimizers, query_len, target_len, params.kmer_size,
            params.compute_divergence },
        PrefilterStage<Alphabet>{
            {}, params, query_seq, target_seq, &output_collection },
        AlignStage<kernel_t<BAA>, Alphabet>{
            {}, params, &scoring_matrix, query_seq, target_seq,
            params.do_cigar || params.compute_edit_distance,
            alignment_proof_dst, host_timing_dst },
        ExactEditFilterStage{{}, params},
        PrimarySecondaryStage{ {}, params.mask_level, params.mask_len, sub_diff(params) },
        SecondaryFilterStage{ {}, params.pri_ratio, min_diff(params), params.best_n },
        MapqStage{ {}, params, params.compute_mapq },
        SinkStage{ &output_collection, query_len, target_len, host_timing_dst }
    );

    if constexpr (EP == EntryPoint::FromSequence) {
        MinimizerStage<MinimizerT, kernel_t<MA>, Alphabet> head{
            {}, query_seq, params.window_size, params.kmer_size, params,
            &output_collection, host_timing_dst };
        return head.template attach<Cep>(std::move(tail));
    } else {
        return tail;
    }
}

// =============================================================================
// 20.4 pipeline_run_impl_batch — batched leaf dispatcher
// =============================================================================

/**
 * @brief Run the pipeline over N queries against one shared target index.
 *
 * Opens a single outer `Arena::Chain` scope for the whole batch and a nested
 * inner scope per query so each query's slabs are reclaimed before the next
 * iteration's pipeline is built.  Per-query results are appended to
 * `out_per_query` in input order.
 */
/// Return whether the resolved statement asks for any full biological Vulkan
/// component. `vulkan_enabled` is deliberately absent: it is only the runtime
/// capability gate, and enabling that gate must not turn an otherwise CPU/SIMD
/// tuple into a full GPU-selected statement.  The rank prefilter is also absent:
/// it is an explicitly requested, self-contained accelerator inside the generic
/// pipeline and does not authorize GPU-to-CPU fallback for minimization,
/// chaining, or alignment. The cache toggle is likewise only policy for a
/// selected seed path.
[[nodiscard]] inline bool vulkan_components_requested(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return params.vulkan_seed_hits_enabled
        || params.vulkan_fused_frontend_enabled
        || params.vulkan_gpu_index_build_enabled
        || params.vulkan_extend_enabled
        || params.minimizer_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::
                   VULKAN_MINIMAP_INT
        || params.chaining_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::
                   VULKAN_TILED_DP;
}

/// Return whether any Vulkan work can execute for this statement.  This is a
/// thread-placement/runtime-lifetime predicate, not biological route
/// admission: a standalone rank prefilter is allowed inside the generic
/// CPU/SIMD pipeline while `vulkan_components_requested()` remains false.
[[nodiscard]] inline bool vulkan_accelerator_requested(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return vulkan_components_requested(params)
        || (params.prefilter_enabled
            && params.prefilter_backend ==
               ::bioseq::prefilter_stats::PrefilterBackendType::VULKAN);
}

/// Exact statement shape that the current dedicated resident GPU route can
/// execute without entering any generic CPU/SIMD biological stage. A selected
/// tuple outside this shape must fail before generic pipeline construction.
[[nodiscard]] inline bool complete_all_gpu_tuple_selected(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return params.vulkan_enabled
        && params.vulkan_seed_hits_enabled
        && params.vulkan_fused_frontend_enabled
        && params.vulkan_gpu_index_build_enabled
        && params.minimizer_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::
                   VULKAN_MINIMAP_INT
        && params.chaining_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::
                   VULKAN_TILED_DP
        && params.base_alignment_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::
                   BLOCK_ALIGNER
        && !params.compute_divergence
        && !params.compute_mapq
        && !params.compute_edit_distance
        && !params.exact_edit_filter_enabled
        && !params.prefilter_enabled
        && params.max_occ >= 0
        && (!params.do_cigar || params.vulkan_extend_enabled);
}

/// Validate the target-format and algorithm tuple admitted by the dedicated
/// persisted Shape-V5 GPU preparation route. The caller supplies the live
/// supported-alphabet pack size so a legitimate zero-based alphabet code is
/// distinguished from an out-of-range code.
[[nodiscard]] inline bool persisted_shape_v5_gpu_preparation_tuple_valid(
    const std::uint8_t alphabet_code,
    const std::size_t supported_alphabet_count,
    const std::uint8_t storage_unit_byte_width,
    const ::bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend backend,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return static_cast<std::size_t>(alphabet_code) < supported_alphabet_count
        && (storage_unit_byte_width == 1u
            || storage_unit_byte_width == 2u
            || storage_unit_byte_width == 4u
            || storage_unit_byte_width == 8u)
        && params.kmer_size == 15
        && params.window_size == 10
        && backend == ::bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::
               VulkanSegment
        && complete_all_gpu_tuple_selected(params);
}

// DOC0310 §16 — the Phase C frontend's GPU target-index build replaces the CPU
// `MinimizerHashmapFragmentIndex` build for this exact (minimizer, chaining)
// leaf. Runtime-gated; both flags plus the matching algorithm selectors must be
// set. The runtime device check is done at the call site.
[[nodiscard]] inline bool phase_c_index_build_active(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return params.vulkan_enabled
        && params.vulkan_fused_frontend_enabled
        && params.vulkan_gpu_index_build_enabled
        && params.minimizer_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::VULKAN_MINIMAP_INT
        && params.chaining_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::VULKAN_TILED_DP;
}

[[nodiscard]] inline bool phase_c_coordinate_only_active(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return params.vulkan_enabled
        && params.vulkan_seed_hits_enabled
        && params.vulkan_fused_frontend_enabled
        && params.minimizer_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::VULKAN_MINIMAP_INT
        && params.chaining_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::VULKAN_TILED_DP
        && !params.do_cigar
        && !params.compute_divergence
        && !params.compute_mapq
        && !params.compute_edit_distance
        && !params.exact_edit_filter_enabled
        && !params.prefilter_enabled
        && params.max_occ >= 0;
}

struct PhaseCQueryGateStorage {
    static constexpr unsigned gate_bits =
        bioseq::gpu::vulkan::kPhaseCQueryGateBits;

    PhaseCQueryGateStorage() = default;
    PhaseCQueryGateStorage(const PhaseCQueryGateStorage&) = delete;
    PhaseCQueryGateStorage& operator=(const PhaseCQueryGateStorage&) = delete;
    PhaseCQueryGateStorage(PhaseCQueryGateStorage&&) = delete;
    PhaseCQueryGateStorage& operator=(PhaseCQueryGateStorage&&) = delete;

    // The only mutable owner is consumed at freeze.  Afterwards every user has
    // a shared_ptr<const vector>; no mutable alias can alter a published gate.
    mutable std::shared_ptr<std::vector<std::uint32_t>> mutable_words{};
    mutable std::shared_ptr<const std::vector<std::uint32_t>> frozen_words{};
    mutable bool frozen = false;
    std::uint32_t mask = 0u;

    void ensure_allocated() {
        if (frozen) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                "Phase C query gate mutation attempted after freeze");
        }
        if (mutable_words) return;
        const std::uint64_t bit_count = std::uint64_t{1} << gate_bits;
        mask = static_cast<std::uint32_t>(bit_count - 1u);
        mutable_words = std::make_shared<std::vector<std::uint32_t>>(
            static_cast<std::size_t>(bit_count / 32u), 0u);
    }

    template <class MinimizerT>
    void add(std::span<const MinimizerT> minimizers) {
        ensure_allocated();
        cached_identity.reset();
        for (const MinimizerT& minimizer : minimizers) {
            const std::uint64_t bit = minimizer.kmer() & mask;
            (*mutable_words)[static_cast<std::size_t>(bit >> 5u)] |=
                std::uint32_t{1} << (bit & 31u);
        }
    }

    [[nodiscard]] std::shared_ptr<const std::vector<std::uint32_t>>
    freeze_words() const noexcept {
        if (!frozen) {
            frozen_words =
                std::shared_ptr<const std::vector<std::uint32_t>>{
                    std::move(mutable_words)};
            frozen = true;
        }
        return frozen_words;
    }

    [[nodiscard]] bioseq::gpu::vulkan::VulkanSketchQueryGate
    vulkan_gate() const noexcept
    {
        const auto gate_identity = identity();
        const auto owner = freeze_words();
        return bioseq::gpu::vulkan::VulkanSketchQueryGate{
            owner
                ? std::span<const std::uint32_t>{owner->data(), owner->size()}
                : std::span<const std::uint32_t>{},
            mask,
            bioseq::gpu::vulkan::VulkanSketchQueryGateIdentity{
                .enabled = gate_identity.enabled,
                .word_count = gate_identity.word_count,
                .mask = gate_identity.mask,
                .digest_lo = gate_identity.digest_lo,
                .digest_hi = gate_identity.digest_hi,
                .canonical_words = owner,
            },
            true};
    }

    struct Identity {
        bool enabled = false;
        std::uint32_t word_count = 0;
        std::uint32_t mask = 0;
        std::uint64_t digest_lo = 0;
        std::uint64_t digest_hi = 0;
        std::uint64_t set_bits = 0;
    };
    mutable std::optional<Identity> cached_identity{};

    /// Canonical 128-bit digest over little-endian u32 gate words. The cache is
    /// also bound once per wave, so this is identity metadata, not permission
    /// to share a filtered resident across unrelated waves.
    [[nodiscard]] Identity identity() const noexcept {
        const auto owner = freeze_words();
        if (cached_identity) return *cached_identity;
        if (!owner) {
            cached_identity = Identity{};
            return *cached_identity;
        }
        std::uint64_t lo = 1469598103934665603ULL;
        std::uint64_t hi = 0x9e3779b97f4a7c15ULL;
        std::uint64_t set_bits = 0u;
        for (const std::uint32_t word : *owner) {
            set_bits += std::popcount(word);
            for (unsigned shift = 0u; shift < 32u; shift += 8u) {
                const std::uint8_t byte = static_cast<std::uint8_t>(word >> shift);
                lo = (lo ^ byte) * 1099511628211ULL;
                hi ^= static_cast<std::uint64_t>(byte)
                    + 0x9e3779b97f4a7c15ULL + (hi << 6u) + (hi >> 2u);
                hi *= 0xbf58476d1ce4e5b9ULL;
            }
        }
        cached_identity = Identity{
            .enabled = true,
            .word_count = static_cast<std::uint32_t>(owner->size()),
            .mask = mask,
            .digest_lo = lo,
            .digest_hi = hi,
            .set_bits = set_bits,
        };
        return *cached_identity;
    }

    [[nodiscard]] std::shared_ptr<const std::vector<std::uint32_t>>
    canonical_words() const noexcept {
        return freeze_words();
    }
};

inline void bind_phase_c_query_gate_identity(
    bioseq::indexing::GpuMinimizerIndexCache* cache,
    const PhaseCQueryGateStorage& gate,
    const PhaseCQueryGateStorage::Identity& gate_identity,
    bioseq::indexing::GpuIndexCacheIdentity& identity)
{
    identity.gate_enabled = gate_identity.enabled;
    identity.gate_word_count = gate_identity.word_count;
    identity.gate_mask = gate_identity.mask;
    identity.gate_digest_lo = gate_identity.digest_lo;
    identity.gate_digest_hi = gate_identity.digest_hi;
    identity.gate_canonical_words = gate.canonical_words();
    if (cache == nullptr) return;
    using Cache = bioseq::indexing::GpuMinimizerIndexCache;
    auto bound = cache->bind_gate_once(Cache::GateIdentity{
        .enabled = gate_identity.enabled,
        .word_count = gate_identity.word_count,
        .mask = gate_identity.mask,
        .digest_lo = gate_identity.digest_lo,
        .digest_hi = gate_identity.digest_hi,
        .canonical_words = gate.canonical_words(),
    });
    if (!bound) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
            "Phase C GPU index cache gate binding rejected ({})",
            bound.error() == Cache::GateBindError::identity_mismatch
                ? "identity_mismatch"
                : bound.error() == Cache::GateBindError::invalid_identity
                    ? "invalid_identity" : "cache_not_empty"));
    }
}

/// Freeze a query gate behind its immutable canonical owner once per wave.
/// Segment loops reuse the returned value, so they neither alias mutable words
/// nor re-hash/copy the 8 MiB gate for every target window.
[[nodiscard]] inline bioseq::gpu::vulkan::VulkanSketchQueryGate
freeze_phase_c_query_gate(
    bioseq::gpu::vulkan::VulkanSketchQueryGate gate)
{
    auto identity = gate.canonical_identity();
    if (!identity.well_formed()) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(
            "Phase C query gate has a malformed canonical identity");
    }
    bioseq::gpu::vulkan::VulkanSketchQueryGate frozen{};
    frozen.mask = identity.mask;
    frozen.identity = std::move(identity);
    frozen.identity_valid = true;
    if (frozen.identity.canonical_words) {
        frozen.words = std::span<const std::uint32_t>{
            frozen.identity.canonical_words->data(),
            frozen.identity.canonical_words->size()};
    }
    return frozen;
}

/// Bind a segmented adoption key to the runtime build's authoritative frozen
/// gate identity, after proving it is exactly the gate supplied to the build.
/// Deriving a default/ungated key for a gated build is an evidence mismatch.
inline void bind_verified_gpu_segment_gate_identity(
    bioseq::indexing::GpuMinimizerIndexCacheKey& key,
    const bioseq::gpu::vulkan::GpuBuiltIndexGateIdentity& expected,
    const bioseq::gpu::vulkan::GpuBuiltIndexGateIdentity& built)
{
    if (!expected.well_formed() || !built.well_formed()
        || !built.exact_equals(expected)) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(
            "segmented GPU target-index gate identity mismatch");
    }
    key.gate_enabled = built.enabled;
    key.gate_word_count = built.word_count;
    key.gate_mask = built.mask;
    key.gate_digest_lo = built.digest_lo;
    key.gate_digest_hi = built.digest_hi;
    key.gate_canonical_words = built.canonical_words;
}

[[nodiscard]] inline MatchResult phase_c_coordinate_to_match_result(
    const bioseq::gpu::vulkan::GpuCoordinateOnlyResultWire& coord,
    sequence_size_t query_len,
    sequence_size_t target_len)
{
    return MatchResult{
        .query_len = query_len,
        .query_start = coord.query_start,
        .query_end = coord.query_end,
        .is_reverse = coord.is_reverse != 0u,
        .target_len = target_len,
        .target_start = coord.target_start,
        .target_end = coord.target_end,
        .num_matches = coord.match_length,
        .block_length = coord.block_length,
        .mapq = 0,
        .chain_score = coord.chain_score,
        .dp_score = 0,
        .dp_max_score = 0,
        .seed_count = coord.seed_count,
        .secondary_chain_score = 0,
        .divergence = -1.0f,
        .is_primary = true,
        .has_cigar = false,
        .cigar = {},
    };
}

// DOC0271 Phase 1 §5.3 — the deferred-align parallel pipeline template
// (`pipeline_run_impl_batch_deferred_align`) and its supporting types
// (`PerQueryAlignState`, `AlignCollectStage`, `align_regions_batched_cross_query`)
// were removed. The two function bodies were 98% identical and LTO already
// DCE'd the deferred body in every linked binary (no production caller sets
// `params.cross_query_batched = true`), so the post-LTO `__TEXT` is unchanged.
// The pre-LTO IR cost — paid in pipe.cppm's TU compile RSS — is what motivates
// the deletion: every `BAA = KSW2_BATCHED` leaf no longer instantiates a
// duplicate ~6 KB body. See DOC0271 §4.3 + §5.3 Option B for the rationale.
template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          sequence_alphabet Alphabet,
          class MinimizerT, class IndexT>
[[nodiscard]] std::vector<MatchResultCollection> pipeline_run_impl_batch(
    std::span<const bioseq::storage::basic_lazy_sequence_view<Alphabet>> query_seqs,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    std::span<const std::vector<MinimizerT>>    query_minimizers_per_query,
    const IndexT&                               target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    bioseq::indexing::GpuMinimizerIndexCache* external_gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false)
{
    std::optional<context::context>                 owned_context;
    std::optional<context::installed_context_scope> install;
    if (context::current_or_null() == nullptr) {
        owned_context.emplace();
        install.emplace(*owned_context);
    }
    auto outer_chain_scope = context::current().push<context::Arena::Chain>();

    bioseq_log<DBG>("[bioseq] pipe: pipeline_run_impl_batch entry — Cep(L={},K={},W={},fallback={}) "
            "BAA={} CA={} MA={} EP={} queries={} target_bp={}",
            Cep::sequence_len_log2, Cep::kmer_len, Cep::window_len,
            Cep::generic_fallback ? 1 : 0,
            static_cast<int>(BAA), static_cast<int>(CA), static_cast<int>(MA),
            static_cast<int>(EP), query_seqs.size(), target_seq.bp_size());

    const sequence_size_t target_len = target_seq.bp_size();
    const auto& scoring_matrix = thread_local_scoring_matrix<Alphabet>(params);

    // DOC0307 §4.4/§5.2 — one GPU index cache for the whole batch wave, shared
    // across queries that look up the same target index. When a Vulkan seed
    // path is selected, runtime/device failure is reported before scalar work
    // can masquerade as a GPU row.
    std::optional<bioseq::indexing::GpuMinimizerIndexCache> owned_gpu_index_cache;
    if (external_gpu_index_cache == nullptr
        && params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
        bioseq::gpu::vulkan::Runtime& gpu_rt = bioseq::gpu::vulkan::Runtime::instance();
        std::uint64_t limit = bioseq::gpu::vulkan::hardware_device_available(gpu_rt)
            ? gpu_rt.features().device_local_bytes / 2u
            : (std::uint64_t{512} << 20);
        if (limit < (std::uint64_t{512} << 20)) limit = std::uint64_t{512} << 20;
        if (limit > (std::uint64_t{4} << 30)) limit = std::uint64_t{4} << 30;
        owned_gpu_index_cache.emplace(limit);
    }
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache_ptr =
        external_gpu_index_cache != nullptr
            ? external_gpu_index_cache
            : (owned_gpu_index_cache ? &*owned_gpu_index_cache : nullptr);

    // DOC0310 §12/§16 — build the target minimizer index on the GPU and inject
    // it into the wave cache under the key the SeedHitStage will compute, so the
    // CPU `MinimizerHashmapFragmentIndex` build (skipped upstream in
    // `minimap2_port` for this leaf) is never paid. If the device build declines
    // this selected GPU path fails loudly.
    PhaseCQueryGateStorage phase_c_query_gate;
    // A production target index is complete and query-independent.  Keep the
    // gate type for explicitly named direct-matching experiments, but publish
    // the ordinary/no-index build under the canonical disabled identity.
    const auto phase_c_gate_proof = phase_c_query_gate.identity();
    if (phase_c_index_build_active(params)) {
        bind_phase_c_query_gate_identity(
            gpu_index_cache_ptr, phase_c_query_gate, phase_c_gate_proof,
            gpu_index_identity);
    }

    const IndexT* effective_index_ptr = &target_index;
    bool vulkan_target_index_built = false;
    bioseq::gpu::vulkan::GpuBuiltIndexOwnershipProof
        vulkan_target_index_ownership{};
    if constexpr (std::same_as<std::remove_cvref_t<IndexT>,
                  bioseq::indexing::MinimizerHashmapFragmentIndex>) {
        if (gpu_index_cache_ptr != nullptr && phase_c_index_build_active(params)
            && allow_gpu_target_index_build && target_index.table_empty()
            && target_len > 0) {
            auto& gpu_rt = bioseq::gpu::vulkan::Runtime::instance();
            bool injected = false;
            bioseq::gpu::vulkan::VulkanSketchDecline decline{};
            auto built = bioseq::gpu::vulkan::build_gpu_index_on_device<MinimizerT>(
                target_seq,
                static_cast<std::size_t>(params.window_size),
                static_cast<std::size_t>(params.kmer_size),
                phase_c_query_gate.vulkan_gate(),
                &decline);
            if (!built) {
                throw_vulkan_sketch_declined(
                    "Phase C GPU target-index build", decline);
            }
            auto key = bioseq::indexing::make_gpu_index_cache_key(
                target_index, gpu_index_identity);
            auto adopted = bioseq::indexing::adopt_gpu_built_index(
                gpu_rt, std::move(*built), key);
            if (!adopted) {
                throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
                    "Phase C GPU target-index adoption rejected ({})",
                    bioseq::indexing::gpu_built_index_adoption_error_name(
                        adopted.error())));
            }
            auto resident = std::move(*adopted);
            vulkan_target_index_ownership = resident->ownership;
            resident->target_len = target_len;
            auto inserted = gpu_index_cache_ptr->insert_built(key, resident);
            if (!inserted) {
                throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
                    "Phase C GPU target-index cache insertion rejected ({})",
                    static_cast<unsigned>(inserted.error())));
            }
            injected = true;
            vulkan_target_index_built = true;
            bioseq_log<ERR>("[phase_c] GPU index injected: slot_mask={} pos={} target_len={}",
                            resident->slot_mask, resident->position_count, target_len);
        }
    }
    const IndexT& effective_index = *effective_index_ptr;

    // Empty proto for the FromSequence path's unused minimizers argument.
    static const std::vector<MinimizerT> empty_minimizers_proto{};

    auto run_one_query =
        [target_len, &query_seqs, &query_minimizers_per_query, target_seq,
         &effective_index, &scoring_matrix, &params, gpu_index_cache_ptr,
         gpu_index_identity, vulkan_target_index_built,
         vulkan_target_index_ownership, phase_c_gate_proof](std::size_t qi) {
        const auto query_seq = query_seqs[qi];
        const sequence_size_t query_len = query_seq.bp_size();

        auto per_query_chain_scope = context::current().push<context::Arena::Chain>();

        MatchResultCollection out;
        if (vulkan_target_index_built
            && phase_c_target_build_proof_owner_query(qi)) {
            PartialVulkanExecutionProof proof{};
            proof.target_sketch_dispatches = 1;
            proof.target_index_build_dispatches = 1;
            proof.gpu_index_count_phase_admissions =
                vulkan_target_index_ownership.count_phase_admissions;
            proof.gpu_index_main_peak_admissions =
                vulkan_target_index_ownership.main_peak_admissions;
            proof.gpu_index_resident_transfers =
                vulkan_target_index_ownership.resident_transfers;
            proof.gpu_index_adoption_rereserve_attempts =
                vulkan_target_index_ownership.adoption_rereserve_attempts;
            proof.gpu_index_adoption_successes =
                vulkan_target_index_ownership.adoption_successes;
            proof.gpu_index_owned_resident_bytes =
                vulkan_target_index_ownership.resident_bytes;
            proof.gpu_index_gate_enabled =
                vulkan_target_index_ownership.query_gate_enabled;
            proof.gpu_index_gate_word_count =
                vulkan_target_index_ownership.query_gate_word_count;
            proof.gpu_index_gate_mask =
                vulkan_target_index_ownership.query_gate_mask;
            proof.gpu_index_gate_set_bits = phase_c_gate_proof.set_bits;
            proof.gpu_index_gate_digest_lo =
                vulkan_target_index_ownership.query_gate_digest_lo;
            proof.gpu_index_gate_digest_hi =
                vulkan_target_index_ownership.query_gate_digest_hi;
            proof.gpu_index_gate_count_dispatches =
                vulkan_target_index_ownership.gate_count_dispatches;
            proof.gpu_index_gated_emit_dispatches =
                vulkan_target_index_ownership.gated_emit_dispatches;
            proof.gpu_index_ungated_emit_dispatches =
                vulkan_target_index_ownership.ungated_emit_dispatches;
            proof.gpu_index_exact_count_dispatches =
                vulkan_target_index_ownership.exact_count_dispatches;
            proof.gpu_index_exact_count_prefix_scan_dispatches =
                vulkan_target_index_ownership.
                    exact_count_prefix_scan_dispatches;
            proof.gpu_index_exact_count_control_readbacks =
                vulkan_target_index_ownership.
                    exact_count_control_readbacks;
            proof.gpu_index_deterministic_tile_emit_dispatches =
                vulkan_target_index_ownership.
                    deterministic_tile_emit_dispatches;
            proof.gpu_index_atomic_append_dispatches =
                vulkan_target_index_ownership.atomic_append_dispatches;
            proof.gpu_index_emit_validation_control_readbacks =
                vulkan_target_index_ownership.
                    emit_validation_control_readbacks;
            proof.gpu_index_counted_minimizer_count =
                vulkan_target_index_ownership.counted_minimizer_count;
            proof.gpu_index_verified_emit_count =
                vulkan_target_index_ownership.verified_emit_count;
            proof.gpu_index_count_emit_parity_verified =
                vulkan_target_index_ownership.count_emit_parity_verified;
            proof.gpu_index_target_minimizer_capacity_estimate =
                vulkan_target_index_ownership.
                    target_minimizer_capacity_estimate;
            proof.gpu_index_capacity_estimate_underestimated =
                vulkan_target_index_ownership.
                    capacity_estimate_underestimated;
            proof.gpu_index_arena_trimmed_before_permit_shrink =
                vulkan_target_index_ownership.
                    arena_trimmed_before_permit_shrink;
            proof.gpu_index_retained_minimizer_count =
                vulkan_target_index_ownership.retained_minimizer_count;
            proof.gpu_index_distinct_minimizer_count =
                vulkan_target_index_ownership.distinct_minimizer_count;
            out.record_partial_vulkan_execution_proof(proof);
        }
        out.results.reserve(detail::estimated_result_count(query_len, target_len, params));

        const auto& mins_for_this_query = (EP == EntryPoint::WithPrebuiltMinimizers)
            ? query_minimizers_per_query[qi]
            : empty_minimizers_proto;

        // Short-circuit early-empty cases matching the legacy entry's
        // returns at `minimap_port.cppm:2313` and :2325.
        if (query_len == 0 || target_len == 0) {
            return out;
        }

        auto pipeline =
            make_minimap_pipeline<Cep, BAA, CA, MA, EP, Alphabet,
                                  MinimizerT, IndexT>(
            query_seq, target_seq, mins_for_this_query, effective_index,
            scoring_matrix, out, params, query_len, target_len,
            gpu_index_cache_ptr, gpu_index_identity);

        if constexpr (EP == EntryPoint::WithPrebuiltMinimizers) {
            pipeline.on_batch(std::span<const MinimizerT>{
                mins_for_this_query.data(), mins_for_this_query.size()});
        } else {
            // FromSequence: MinimizerStage's process ignores the input and
            // sketches the cached query_seq_ pointer instead; pass an empty
            // span of bytes as a placeholder.
            pipeline.on_batch(std::span<const std::byte>{});
        }
        pipeline.on_completed();
        return out;
    };

    std::vector<MatchResultCollection> out_per_query;
    const bool may_parallel_batch =
        !vulkan_accelerator_requested(params)
        && bioseq::parallelism::task_parallel(
            bioseq::parallelism::GlobalPool::instance().mode())
        && query_seqs.size()
            >= bioseq::parallelism::TaskParallelMinTriggers::MIN_QUERY_COUNT
        && lazy_is_worker_shareable(target_seq)
        && std::ranges::all_of(query_seqs,
               [](const auto query) {
                   return lazy_is_worker_shareable(query);
               });

    if (may_parallel_batch) {
        out_per_query.resize(query_seqs.size());
        bioseq::parallelism::parallel_for_pipeline(
            0, query_seqs.size(), 1,
            bioseq::parallelism::TaskParallelMinTriggers::MIN_QUERY_COUNT,
            bioseq::parallelism::TaskParallelRegion::query_batch,
            [&out_per_query, &run_one_query](std::size_t lo, std::size_t hi) {
                for (std::size_t qi = lo; qi < hi; ++qi) {
                    out_per_query[qi] = run_one_query(qi);
                }
            });
    } else {
        out_per_query.reserve(query_seqs.size());
        for (std::size_t qi = 0; qi < query_seqs.size(); ++qi) {
            out_per_query.push_back(run_one_query(qi));
        }
    }

    // DOC0307 §4.4 — release the wave's resident GPU index handles.
    if (owned_gpu_index_cache) {
        owned_gpu_index_cache->clear();
    }
    return out_per_query;
}

// DOC0271 Phase 1 §5.3 — the entire cross-query deferred-align scheduling shape
// (including `pipeline_run_impl_batch_deferred_align`, `PerQueryAlignState`,
// `AlignCollectStage`, and `align_regions_batched_cross_query`) was deleted.
// Its only structural difference from `pipeline_run_impl_batch` was one extra
// `Arena::Chain` push for a not-yet-implemented cross-query fill accumulator;
// the cross-query helper itself was a pass-through over per-query
// `align_regions_batched<…>` calls. Restoring the shape later means re-adding
// the four declarations above; the runtime semantics are now `cross_query_batched`-
// flag-independent.

/**
 * @brief Single-query convenience wrapper around `pipeline_run_impl_batch`.
 */
template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          sequence_alphabet Alphabet,
          class MinimizerT, class IndexT,
          class QueryMinimizers = std::vector<MinimizerT>>
[[nodiscard, gnu::always_inline]] inline MatchResultCollection pipeline_run_impl(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const QueryMinimizers&            query_minimizers,
    const IndexT&                     target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false,
    TransientTargetIndexReleaseAuthority
        transient_target_index_owner = {})
{
    std::optional<context::context>                 owned_context;
    std::optional<context::installed_context_scope> install;
    if (context::current_or_null() == nullptr) {
        owned_context.emplace();
        install.emplace(*owned_context);
    }
    // The locally owned cache must outlive PostSeed's raw cache pointer, while
    // PostSeed itself must outlive every Arena::Chain that backs an admitted
    // anchor slab. Reverse destruction therefore requires this exact order.
    std::optional<bioseq::indexing::GpuMinimizerIndexCache>
        owned_gpu_index_cache;
    // Declared before the Arena::Chain scope so any exact retained anchor
    // permit is released only after that arena has physically unwound.
    std::optional<PostSeedOwnedLifetime> post_seed_owned_lifetime;
    auto outer_chain_scope = context::current().push<context::Arena::Chain>();

    const sequence_size_t target_len = target_seq.bp_size();
    const sequence_size_t query_len = query_seq.bp_size();
    const auto& scoring_matrix = thread_local_scoring_matrix<Alphabet>(params);

    if (gpu_index_cache == nullptr
        && params.vulkan_enabled && params.vulkan_seed_hits_enabled
        && !(gpu_index_identity.source_direct_sorted_dense
            && gpu_index_identity.canonical_empty)) {
        bioseq::gpu::vulkan::Runtime& gpu_rt = bioseq::gpu::vulkan::Runtime::instance();
        std::uint64_t limit = bioseq::gpu::vulkan::hardware_device_available(gpu_rt)
            ? gpu_rt.features().device_local_bytes / 2u
            : (std::uint64_t{512} << 20);
        if (limit < (std::uint64_t{512} << 20)) limit = std::uint64_t{512} << 20;
        if (limit > (std::uint64_t{4} << 30)) limit = std::uint64_t{4} << 30;
        owned_gpu_index_cache.emplace(limit);
    }
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache_ptr =
        gpu_index_cache != nullptr
            ? gpu_index_cache
            : (owned_gpu_index_cache ? &*owned_gpu_index_cache : nullptr);

    PhaseCQueryGateStorage phase_c_query_gate;
    // Fair no-index semantics require the full query-independent target index.
    const auto phase_c_gate_proof = phase_c_query_gate.identity();
    if (phase_c_index_build_active(params)) {
        bind_phase_c_query_gate_identity(
            gpu_index_cache_ptr, phase_c_query_gate, phase_c_gate_proof,
            gpu_index_identity);
    }

    const IndexT* effective_index_ptr = &target_index;
    bool vulkan_target_index_built = false;
    bioseq::gpu::vulkan::GpuBuiltIndexOwnershipProof
        vulkan_target_index_ownership{};
    if constexpr (std::same_as<std::remove_cvref_t<IndexT>,
                  bioseq::indexing::MinimizerHashmapFragmentIndex>) {
        if (gpu_index_cache_ptr != nullptr && phase_c_index_build_active(params)
            && allow_gpu_target_index_build && target_index.table_empty()
            && target_len > 0) {
            auto& gpu_rt = bioseq::gpu::vulkan::Runtime::instance();
            bool injected = false;
            bioseq::gpu::vulkan::VulkanSketchDecline decline{};
            auto built = bioseq::gpu::vulkan::build_gpu_index_on_device<MinimizerT>(
                target_seq,
                static_cast<std::size_t>(params.window_size),
                static_cast<std::size_t>(params.kmer_size),
                phase_c_query_gate.vulkan_gate(),
                &decline);
            if (!built) {
                throw_vulkan_sketch_declined(
                    "Phase C GPU target-index build", decline);
            }
            auto key = bioseq::indexing::make_gpu_index_cache_key(
                target_index, gpu_index_identity);
            auto adopted = bioseq::indexing::adopt_gpu_built_index(
                gpu_rt, std::move(*built), key);
            if (!adopted) {
                throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
                    "Phase C GPU target-index adoption rejected ({})",
                    bioseq::indexing::gpu_built_index_adoption_error_name(
                        adopted.error())));
            }
            auto resident = std::move(*adopted);
            vulkan_target_index_ownership = resident->ownership;
            resident->target_len = target_len;
            auto inserted = gpu_index_cache_ptr->insert_built(key, resident);
            if (!inserted) {
                throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
                    "Phase C GPU target-index cache insertion rejected ({})",
                    static_cast<unsigned>(inserted.error())));
            }
            injected = true;
            vulkan_target_index_built = true;
            bioseq_log<ERR>("[phase_c] GPU index injected: slot_mask={} pos={} target_len={}",
                            resident->slot_mask, resident->position_count, target_len);
        }
    }
    const IndexT& effective_index = *effective_index_ptr;

    MatchResultCollection out;
    if (vulkan_target_index_built) {
        PartialVulkanExecutionProof proof{};
        proof.target_sketch_dispatches = 1;
        proof.target_index_build_dispatches = 1;
        proof.gpu_index_count_phase_admissions =
            vulkan_target_index_ownership.count_phase_admissions;
        proof.gpu_index_main_peak_admissions =
            vulkan_target_index_ownership.main_peak_admissions;
        proof.gpu_index_resident_transfers =
            vulkan_target_index_ownership.resident_transfers;
        proof.gpu_index_adoption_rereserve_attempts =
            vulkan_target_index_ownership.adoption_rereserve_attempts;
        proof.gpu_index_adoption_successes =
            vulkan_target_index_ownership.adoption_successes;
        proof.gpu_index_owned_resident_bytes =
            vulkan_target_index_ownership.resident_bytes;
        proof.gpu_index_gate_enabled =
            vulkan_target_index_ownership.query_gate_enabled;
        proof.gpu_index_gate_word_count =
            vulkan_target_index_ownership.query_gate_word_count;
        proof.gpu_index_gate_mask =
            vulkan_target_index_ownership.query_gate_mask;
        proof.gpu_index_gate_set_bits = phase_c_gate_proof.set_bits;
        proof.gpu_index_gate_digest_lo =
            vulkan_target_index_ownership.query_gate_digest_lo;
        proof.gpu_index_gate_digest_hi =
            vulkan_target_index_ownership.query_gate_digest_hi;
        proof.gpu_index_gate_count_dispatches =
            vulkan_target_index_ownership.gate_count_dispatches;
        proof.gpu_index_gated_emit_dispatches =
            vulkan_target_index_ownership.gated_emit_dispatches;
        proof.gpu_index_ungated_emit_dispatches =
            vulkan_target_index_ownership.ungated_emit_dispatches;
        proof.gpu_index_exact_count_dispatches =
            vulkan_target_index_ownership.exact_count_dispatches;
        proof.gpu_index_exact_count_prefix_scan_dispatches =
            vulkan_target_index_ownership.exact_count_prefix_scan_dispatches;
        proof.gpu_index_exact_count_control_readbacks =
            vulkan_target_index_ownership.exact_count_control_readbacks;
        proof.gpu_index_deterministic_tile_emit_dispatches =
            vulkan_target_index_ownership.deterministic_tile_emit_dispatches;
        proof.gpu_index_atomic_append_dispatches =
            vulkan_target_index_ownership.atomic_append_dispatches;
        proof.gpu_index_emit_validation_control_readbacks =
            vulkan_target_index_ownership.emit_validation_control_readbacks;
        proof.gpu_index_counted_minimizer_count =
            vulkan_target_index_ownership.counted_minimizer_count;
        proof.gpu_index_verified_emit_count =
            vulkan_target_index_ownership.verified_emit_count;
        proof.gpu_index_count_emit_parity_verified =
            vulkan_target_index_ownership.count_emit_parity_verified;
        proof.gpu_index_target_minimizer_capacity_estimate =
            vulkan_target_index_ownership.target_minimizer_capacity_estimate;
        proof.gpu_index_capacity_estimate_underestimated =
            vulkan_target_index_ownership.capacity_estimate_underestimated;
        proof.gpu_index_arena_trimmed_before_permit_shrink =
            vulkan_target_index_ownership.
                arena_trimmed_before_permit_shrink;
        proof.gpu_index_retained_minimizer_count =
            vulkan_target_index_ownership.retained_minimizer_count;
        proof.gpu_index_distinct_minimizer_count =
            vulkan_target_index_ownership.distinct_minimizer_count;
        out.record_partial_vulkan_execution_proof(proof);
    }
    out.results.reserve(detail::estimated_result_count(query_len, target_len, params));
    DirectResidentOperationLedger* const direct_ledger =
        transient_target_index_owner.direct_ledger;
    const bool direct_empty_bypass = direct_ledger != nullptr
        && gpu_index_identity.source_direct_sorted_dense
        && gpu_index_identity.canonical_empty;
    const bool early_zero_consumer_bypass =
        direct_empty_bypass || query_len == 0 || target_len == 0;
    const std::size_t actual_seed_consumers =
        early_zero_consumer_bypass ? 0u : 1u;
    if (direct_ledger != nullptr) {
        direct_ledger->expected_seed_consumers = actual_seed_consumers;
        if (direct_empty_bypass) {
            direct_ledger->empty_bypass = true;
        } else if (early_zero_consumer_bypass) {
            direct_ledger->zero_query_bypasses = 1u;
        }
    }
    if (owned_gpu_index_cache
        || transient_target_index_owner.index != nullptr
        || transient_target_index_owner.operation_owned_gpu_cache != nullptr
        || transient_target_index_owner.direct_ledger != nullptr) {
        // CPU-index release authority is supplied only by `minimap2_port`,
        // which owns that transient value. GPU-cache release authority is
        // independent: even a borrowed persisted CPU index may use and then
        // retire the cache constructed by this invocation. An external cache
        // is always borrowed and is never installed here.
        post_seed_owned_lifetime.emplace(
            actual_seed_consumers,
            owned_gpu_index_cache
                ? &*owned_gpu_index_cache
                : transient_target_index_owner.
                    operation_owned_gpu_cache,
            transient_target_index_owner);
    }

    if (early_zero_consumer_bypass) {
        return out;
    }

    if constexpr (EP == EntryPoint::WithPrebuiltMinimizers
                  && std::same_as<std::remove_cvref_t<IndexT>,
                         bioseq::indexing::MinimizerHashmapFragmentIndex>) {
        // The resident seed-to-coordinate kernel is only profitable once the
        // target index is genuinely resident/GPU-built. Using it on the current
        // VulkanSegment host-materialised sparse segment indexes repeats
        // submit/readback orchestration per segment and is slower than the
        // normal Vulkan seed+chain path (DOC0353 2026-06-25).
        if (phase_c_coordinate_only_active(params)
            && allow_gpu_target_index_build
            && effective_index.table_empty()) {
            auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
            if (!bioseq::gpu::vulkan::hardware_device_available(runtime)) {
                throw_vulkan_declined("Phase C resident seed-to-coordinate");
            }
            auto key = bioseq::indexing::make_gpu_index_cache_key(
                effective_index, gpu_index_identity);
            auto resident_index = gpu_index_cache_ptr == nullptr
                ? nullptr
                : gpu_index_cache_ptr->get_or_upload(
                      runtime, effective_index, key,
                      params.vulkan_index_cache_enabled);
            if (!resident_index) {
                throw_vulkan_declined("Phase C resident seed-to-coordinate index");
            }

            std::span<const MinimizerT> sketch_span{
                query_minimizers.data(), query_minimizers.size()};
            std::optional<bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>>
                device_span{};
            if constexpr (is_minimizer_batch_view_v<QueryMinimizers>) {
                device_span = query_minimizers.device;
            }
            const bioseq::gpu::vulkan::Buffer* device_words =
                (device_span && device_span->words() != nullptr
                 && device_span->count() == sketch_span.size())
                    ? device_span->words()
                    : nullptr;
            bioseq::gpu::vulkan::PhaseCSeedInputs inputs{
                .host_minimizer_bytes = device_words != nullptr
                    ? std::span<const std::byte>{}
                    : std::as_bytes(sketch_span),
                .device_minimizer_words = device_words,
                .slots = &resident_index->slots,
                .positions = &resident_index->positions,
                .target_len = resident_index->target_len,
                .slot_mask = resident_index->slot_mask,
                .position_count = resident_index->position_count,
                .minimizer_count = static_cast<std::uint32_t>(sketch_span.size()),
                .minimizer_word_kind =
                    static_cast<std::uint32_t>(
                        bioseq::gpu::vulkan::minimizer_wire_traits<MinimizerT>::word_kind),
                .minimizer_words32_per_record =
                    bioseq::gpu::vulkan::minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
                .hash_shift =
                    bioseq::gpu::vulkan::minimizer_wire_traits<MinimizerT>::hash_shift,
                .pos_bits =
                    bioseq::gpu::vulkan::minimizer_wire_traits<MinimizerT>::pos_bits,
                .bucket_count = resident_index->bucket_count,
                .target_position_offset = resident_index->target_position_offset,
            };

            auto cp = params.to_chaining_params();
            std::int32_t max_dist_x = cp.max_gap;
            std::int32_t max_dist_y = cp.max_gap;
            if (max_dist_x < cp.bandwidth) max_dist_x = cp.bandwidth;
            if (max_dist_y < cp.bandwidth) max_dist_y = cp.bandwidth;
            const auto chain_key =
                bioseq::chaining::choose_vulkan_chaining_pipeline(runtime.features());
            const auto coord = runtime.run_phase_c_seed_to_coordinate_roundtrip(
                inputs,
                query_len,
                target_len,
                static_cast<std::uint32_t>(params.kmer_size),
                params.max_occ,
                chain_key,
                max_dist_x,
                max_dist_y,
                cp.bandwidth,
                std::min<std::int32_t>(cp.max_chain_iter, 3000),
                cp.gap_penalty,
                cp.skip_penalty,
                cp.max_chain_skip,
                cp.min_chain_score,
                cp.min_seed_count,
                16u);
            if (coord.valid != 0u) {
                out.results.push_back(
                    phase_c_coordinate_to_match_result(coord, query_len, target_len));
            }
            return out;
        }
    }

    if (direct_ledger != nullptr) {
        direct_ledger->pipeline_started = true;
    }
    auto per_query_chain_scope = context::current().push<context::Arena::Chain>();
    auto pipeline =
        make_minimap_pipeline<Cep, BAA, CA, MA, EP, Alphabet,
                              MinimizerT, IndexT, QueryMinimizers>(
        query_seq, target_seq, query_minimizers, effective_index,
        scoring_matrix, out, params, query_len, target_len,
        gpu_index_cache_ptr, gpu_index_identity,
        post_seed_owned_lifetime ? &*post_seed_owned_lifetime : nullptr);

    if constexpr (EP == EntryPoint::WithPrebuiltMinimizers) {
        pipeline.on_batch(query_minimizers);
    } else {
        pipeline.on_batch(std::span<const std::byte>{});
    }
    pipeline.on_completed();
    if (direct_ledger != nullptr) {
        // Seed completion alone is insufficient: this bit is set only after
        // both the batch and the terminal pipeline callback returned normally.
        direct_ledger->pipeline_completed = true;
    }
    return out;
}

// =============================================================================
// DOC0256 / DOC0261 — value_sequence (non-type parameter pack carrier)
// =============================================================================
//
// `value_sequence<T, Vs...>` lives in `bioseq_common.templating` (DOC0261
// §4.3); re-exported here so the existing pipe.cppm body keeps the
// unqualified name. See DOC0261 §12.4.

using ::bioseq::common::templating::value_sequence;

// =============================================================================
// DOC0256 §4 — MinimizerLayout-driven L bucket derivation
// =============================================================================
//
// Replaces the hand-coded `(K, L)` ladders that used to live inside
// `dispatch_pipeline` and `minimap_port`.  The bucket list is computed at
// compile time from the per-K transitions in `MinimizerLayout::storage_bits`,
// so adding a new K only requires editing `kmer_size_buckets` and the
// per-K clause in `storage_bits_for`.

/// Canonical k-mer values the bench/postgres paths instantiate.  The leading
/// entry is the default for non-canonical kmer_size — see kmer_size_default.
///
/// DOC0271 Phase 1 / BIOSEQ_OPTTIMIZATION_LEVEL — the array's SIZE must
/// match its element count, not be hard-coded to 3. A `std::array<size_t, 3>`
/// with only `{15u}` initialised silently zeroes the trailing two slots, and
/// the consteval dispatch table then enumerates K=0, which instantiates
/// `MinimizerRepr<63, 0>` whose `hash_shift` equals `sizeof(storage_t)*8` and
/// trips an undefined-shift constexpr error in `index.cppm:151`. `std::to_array`
/// deduces the array size from the initialiser so the two stay in lockstep.
inline constexpr auto kmer_size_buckets = std::to_array<std::size_t>({
    #if BIOSEQ_OPTTIMIZATION_LEVEL <= 3
    15u
    #else
    15u, 7u, 27u
    #endif
});

/// `bit_width(max(max_bp - 1, 1))` clamped into `[K, 63]` — the smallest
/// `L` such that `2^L > max_bp - 1`, lifted up to `K` so the resulting
/// `MinimizerLayout<L, K>` keeps `pos_bits >= kmer_size`.  At L=63 every K
/// resolves to a non-void storage_t (the 128-bit specialisation handles the
/// upper band), so a clamp on the upper end is sufficient.
[[nodiscard]] constexpr std::size_t round_up_seq_len_log2(
    std::size_t max_bp, std::size_t kmer_size) noexcept
{
    const std::size_t need = max_bp > 1u ? max_bp - 1u : 1u;
    const std::size_t lg   = static_cast<std::size_t>(std::bit_width(need));
    return std::clamp<std::size_t>(lg, kmer_size, 63u);
}

static_assert(round_up_seq_len_log2(0u,            15u) == 15u);
static_assert(round_up_seq_len_log2(1u,            15u) == 15u);
static_assert(round_up_seq_len_log2(std::size_t{1} << 28, 15u) == 28u);
static_assert(round_up_seq_len_log2((std::size_t{1} << 48) + 1u, 15u) == 49u);
static_assert(round_up_seq_len_log2(std::size_t{1} << 62, 15u) == 62u);

namespace detail {

/// Pack-expansion builder for a per-K table of `storage_bits` values keyed
/// by L ∈ [0, 64).  The layout type takes (L, K) as template parameters, so
/// the only way to convert a runtime L into a template argument is to
/// pre-compute every value of `MinimizerLayout<L, K>::storage_bits` for L
/// in the legal range and index into the resulting `std::array`.  All work
/// happens at constant-evaluation time; the dispatcher path never enters
/// this helper.
template <std::size_t K, std::size_t... Ls>
[[nodiscard]] consteval auto make_storage_bits_table_impl(
    std::index_sequence<Ls...>) noexcept
{
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerLayout;
    return std::array<std::size_t, sizeof...(Ls)>{
        MinimizerLayout<Ls, K>::storage_bits...
    };
}

template <std::size_t K>
[[nodiscard]] consteval auto make_storage_bits_table() noexcept {
    return make_storage_bits_table_impl<K>(std::make_index_sequence<64u>{});
}

/// Look up `MinimizerLayout<L, K>::storage_bits` from the per-K table.  The
/// per-K clauses are a *dispatch table*, not a hand-tuned ladder — they exist
/// because the layout requires K as a *template* argument and the consteval
/// helper above can only enumerate L for a fixed compile-time K.  Every K
/// in `kmer_size_buckets` must have a matching clause here.
[[nodiscard]] consteval std::size_t storage_bits_for(
    std::size_t L, std::size_t K) noexcept
{
    if (K == 15u) return make_storage_bits_table<15u>()[L];
    if (K == 7u)  return make_storage_bits_table<7u>()[L];
    if (K == 27u) return make_storage_bits_table<27u>()[L];
    return 0u;
}

/// Compute the breakpoint L values for a given K.  For each L in [K, 63]:
///   - L is emitted if storage_bits_for(L, K) != storage_bits_for(L-1, K),
///     recorded as the largest L of the previous band (i.e. L-1).
///   - L = K is emitted as the lower anchor.
///   - L = 63 is always emitted as the terminal anchor.
/// Max 6 entries possible (initial + up to 3 storage transitions + terminal +
/// headroom).
template <std::size_t K>
[[nodiscard]] consteval auto compute_breakpoints_for() noexcept {
    std::array<std::size_t, 6u> out{};
    std::size_t out_n = 0u;
    std::size_t last_bits = 0u;
    for (std::size_t L = K; L <= 63u; ++L) {
        const std::size_t bits = storage_bits_for(L, K);
        if (bits != last_bits) {
            out[out_n++] = (last_bits == 0u) ? L : L - 1u;
            last_bits = bits;
        }
    }
    out[out_n++] = 63u;
    return std::pair{out, out_n};
}

static_assert(compute_breakpoints_for<15u>().second <= 6u);
static_assert(compute_breakpoints_for<7u>().second  <= 6u);
static_assert(compute_breakpoints_for<27u>().second <= 6u);

/// Work-buffer type for `compute_global_l_buckets`.  Sized 32 (≤ 6
/// breakpoints/K × ≤ 5 kmer sizes = 30; rounded up to a power of two for
/// headroom).
using raw_buf = std::array<std::size_t, 32u>;

/// Free consteval helper — merges compute_breakpoints_for<K>()'s breakpoint
/// list into (raw, raw_n) using the lifted `append_unique_value` primitive.
template <std::size_t K>
consteval void merge_breakpoints_into(
    raw_buf& raw, std::size_t& raw_n) noexcept
{
    constexpr auto bp = compute_breakpoints_for<K>();
    for (std::size_t i = 0u; i < bp.second; ++i) {
        ::bioseq::common::templating::append_unique_value(raw, raw_n, bp.first[i]);
    }
}

/// Pack-expanded union over `kmer_size_buckets` indices.  Threads the
/// accumulator state through `merge_breakpoints_into` (no `[&]` capture)
/// and finishes with the lifted insertion-sort primitive — the
/// dispatcher's smallest-ge lookup needs sorted input.
template <std::size_t... Is>
[[nodiscard]] consteval auto compute_global_l_buckets_impl(
    std::index_sequence<Is...>) noexcept
{
    raw_buf raw{};
    std::size_t raw_n = 0u;
    (merge_breakpoints_into<kmer_size_buckets[Is]>(raw, raw_n), ...);
    ::bioseq::common::templating::sort_in_place_ascending(raw, raw_n);
    return std::pair{raw, raw_n};
}

[[nodiscard]] consteval auto compute_global_l_buckets() noexcept {
    return compute_global_l_buckets_impl(
        std::make_index_sequence<kmer_size_buckets.size()>{});
}

} // namespace detail

/// Materialised dispatcher L bucket list — the union of per-K breakpoints
/// computed at compile time.  The `_n` constant gives the live prefix size;
/// the array itself is sized 32 for headroom.
inline constexpr auto seq_len_log2_buckets_pair = detail::compute_global_l_buckets();
inline constexpr auto seq_len_log2_buckets      = seq_len_log2_buckets_pair.first;
inline constexpr std::size_t seq_len_log2_buckets_n =
    seq_len_log2_buckets_pair.second;
static_assert(seq_len_log2_buckets_n >= 1u,
    "seq_len_log2_buckets must be non-empty");

namespace detail {

/// Materialise `seq_len_log2_buckets` as a `value_sequence` the chain can
/// pack-expand.  Used as the value-list of `seq_len_log2_registry`.
template <std::size_t... Is>
[[nodiscard]] consteval auto make_seq_len_log2_buckets_seq(
    std::index_sequence<Is...>) noexcept
{
    return value_sequence<std::size_t, seq_len_log2_buckets[Is]...>{};
}

using seq_len_log2_buckets_seq = decltype(
    make_seq_len_log2_buckets_seq(
        std::make_index_sequence<seq_len_log2_buckets_n>{}));

/// Materialise `kmer_size_buckets` similarly.  Used as the value-list of
/// `kmer_size_registry`.
template <std::size_t... Is>
[[nodiscard]] consteval auto make_kmer_size_buckets_seq(
    std::index_sequence<Is...>) noexcept
{
    return value_sequence<std::size_t, kmer_size_buckets[Is]...>{};
}

using kmer_size_buckets_seq = decltype(
    make_kmer_size_buckets_seq(
        std::make_index_sequence<kmer_size_buckets.size()>{}));

// DOC0271 Phase 1 §5.1 — per-K seq_len_log2 breakpoint sequence.
//
// Materialise the K-specific breakpoint list (from `compute_breakpoints_for<K>()`)
// as a `value_sequence<std::size_t, ...>`.  Used by `bucket_step_pinned`'s
// K-aware `value_list_for<K>` to iterate only the L breakpoints relevant for
// that K — rather than the global union shape `seq_len_log2_buckets_seq` (which
// causes the §3 explosion).
template <std::size_t K, std::size_t... Is>
[[nodiscard]] consteval auto make_per_k_breakpoints_seq_impl(
    std::index_sequence<Is...>) noexcept
{
    constexpr auto bp = compute_breakpoints_for<K>();
    return value_sequence<std::size_t, bp.first[Is]...>{};
}

template <std::size_t K>
[[nodiscard]] consteval auto make_per_k_breakpoints_seq() noexcept {
    constexpr auto bp = compute_breakpoints_for<K>();
    return make_per_k_breakpoints_seq_impl<K>(std::make_index_sequence<bp.second>{});
}

template <std::size_t K>
using per_k_breakpoints_seq = decltype(make_per_k_breakpoints_seq<K>());

static_assert(per_k_breakpoints_seq<15u>::size() == compute_breakpoints_for<15u>().second);
static_assert(per_k_breakpoints_seq<7u>::size()  == compute_breakpoints_for<7u>().second);
static_assert(per_k_breakpoints_seq<27u>::size() == compute_breakpoints_for<27u>().second);

} // namespace detail

/// Canonical k-mer default — the historical fallback for any non-canonical
/// `params.kmer_size` value.  The chain's `kmer_size_step` pins this when
/// the runtime field is outside `kmer_size_buckets`.
inline constexpr std::size_t kmer_size_default = 15u;
static_assert(kmer_size_default == kmer_size_buckets[0],
    "kmer_size_default must match the leading entry of kmer_size_buckets — "
    "the chain's default-value path expects them in sync.");

// =============================================================================
// DOC0256 §5.1–5.3 / DOC0261 §4.4 — Low-level enum/value dispatch primitives
// =============================================================================
//
// The dispatch primitives now live in `bioseq_common.templating`
// (DOC0261). They are re-exported under the existing names so this
// module's call sites stay unchanged. Codegen is identical to the
// previous in-place definition — every helper carries
// `[[gnu::always_inline]]`.

using ::bioseq::common::templating::enum_dispatch_pack;
using ::bioseq::common::templating::enum_dispatch_pack_void;
using ::bioseq::common::templating::value_dispatch_smallest_ge;
using ::bioseq::common::templating::value_dispatch_exact_with_default;

// =============================================================================
// DOC0256 §5.4 / §5.9 — algorithm_enum_values registry + per-dim selectors
// =============================================================================
//
// Each registry exposes:
//   - `target_t`        — the NTTP type the dispatcher pins.
//   - `default_value`   — pinned when the runtime field is outside the pack.
//   - `value_list`      — `std::integer_sequence<target_t, ...>` of enum/size_t
//                         values the dispatcher iterates.
//   - `read_field(p, args...)` — extracts the target_t value from the runtime
//                         parameter pack (singular method name shared by every
//                         step adapter; the implementation may literally read a
//                         field or compute a derived value).
//
// Adding a new dispatched dimension is one new registry plus one `enum_step` /
// `bucket_step` alias.

template <class Enum>
struct algorithm_enum_values;

template <>
struct algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType> {
    using target_t = ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using enum_t   = target_t;  // legacy alias.
    static constexpr target_t default_value = target_t::SCALAR_MM2;
    using value_list = value_sequence<target_t,
        target_t::SCALAR_MM2,
        target_t::SIMD_MM2,
        target_t::RMQ,
        target_t::VULKAN_TILED_DP>;

    template <class... Args>
    [[nodiscard, gnu::always_inline]] static constexpr target_t read_field(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
        Args&&...) noexcept
    {
        return p.chaining_algorithm;
    }
};

template <>
struct algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType> {
    using target_t = ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
    using enum_t   = target_t;
    static constexpr target_t default_value = target_t::BLOCK_ALIGNER;
    using value_list = value_sequence<target_t,
        target_t::KSW2,
        target_t::BLOCK_ALIGNER,
        target_t::KSW2_BATCHED>;

    template <class... Args>
    [[nodiscard, gnu::always_inline]] static constexpr target_t read_field(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
        Args&&...) noexcept
    {
        return p.base_alignment_algorithm;
    }
};

template <>
struct algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType> {
    using target_t = ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using enum_t   = target_t;
    static constexpr target_t default_value = target_t::SCALAR;
    using value_list = value_sequence<target_t,
        target_t::SCALAR,
        target_t::SIMD_MINIMAP2_SLOW,
        target_t::TWO_STACK_NTHASH,
        target_t::VULKAN_MINIMAP_INT,
        target_t::PAPER_NTHASH_SIMD>;

    template <class... Args>
    [[nodiscard, gnu::always_inline]] static constexpr target_t read_field(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
        Args&&...) noexcept
    {
        return p.minimizer_algorithm;
    }
};

namespace detail {

template <class Enum, Enum V>
[[nodiscard]] consteval bool trait_specialised_for() noexcept {
    return requires { typename algorithm_traits<V>::kernel_type; };
}

template <class Enum, Enum... Vs>
[[nodiscard]] consteval bool all_traits_specialised_for(
    value_sequence<Enum, Vs...>) noexcept
{
    return (trait_specialised_for<Enum, Vs>() && ...);
}

} // namespace detail

static_assert(detail::all_traits_specialised_for(
    algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType>::value_list{}),
    "algorithm_enum_values<ChainingAlgorithmType>::value_list contains a value "
    "without an algorithm_traits<>::kernel_type specialisation.  Add the trait "
    "specialisation alongside the enum value.");
static_assert(detail::all_traits_specialised_for(
    algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType>::value_list{}),
    "algorithm_enum_values<BaseAlignmentAlgorithmType>::value_list contains a "
    "value without an algorithm_traits<>::kernel_type specialisation.");
static_assert(detail::all_traits_specialised_for(
    algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>::value_list{}),
    "algorithm_enum_values<MinimizerAlgorithmType>::value_list contains a "
    "value without an algorithm_traits<>::kernel_type specialisation.");

namespace detail {

/// A type satisfies `has_bp_size` when it exposes a `bp_size()` member
/// returning an unsigned integer.  Constrains `seq_len_log2_registry::read_field`'s
/// first two args so a chain composing `seq_len_log2_step` into a call site
/// lacking `(QR&, TR&)` in those slots produces a localised diagnostic
/// at the chain instantiation point.
template <class T>
concept has_bp_size = requires (const T& x) {
    { x.bp_size() } -> std::convertible_to<std::size_t>;
};

struct seq_len_log2_registry {
    using target_t = std::size_t;
    static constexpr std::size_t default_value = seq_len_log2_buckets[0];
    using value_list = seq_len_log2_buckets_seq;

    template <has_bp_size QR_arg, has_bp_size TR_arg, class... Rest>
    [[nodiscard, gnu::always_inline]] static std::size_t read_field(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        const QR_arg& q, const TR_arg& t,
        Rest&&...) noexcept
    {
        const std::size_t max_bp = std::max(q.bp_size(), t.bp_size());
        return round_up_seq_len_log2(
            max_bp, static_cast<std::size_t>(params.kmer_size));
    }
};

struct kmer_size_registry {
    using target_t = std::size_t;
    static constexpr std::size_t default_value = kmer_size_default;
    using value_list = kmer_size_buckets_seq;

    template <class... Args>
    [[nodiscard, gnu::always_inline]] static constexpr std::size_t read_field(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        Args&&...) noexcept
    {
        return static_cast<std::size_t>(params.kmer_size);
    }
};

/// DOC0271 Phase 1 §5.1 — pinned-aware seq_len_log2 registry.
///
/// Exposes `value_list_for<K, …>` and `read_field_for<K, …>` so the chain's
/// `bucket_step_pinned` can iterate only the L breakpoints for the previously-pinned K.
/// This collapses the 21-cell (L, K) Cartesian product (the global L union × K) into
/// the per-K breakpoint count: `3 + 4 + 2 = 9` cells (K=15: 3 buckets, K=7: 4, K=27: 2).
///
/// Place this step *after* `kmer_size_step` in the chain so `Pinned... = (K, …)`.
struct seq_len_log2_registry_per_k {
    using target_t = std::size_t;

    /// Anchored to K=15's lower bucket boundary (`compute_breakpoints_for<15>()[0]`).
    /// The chain only ever pins `default_value` when `active_v` is false; today every
    /// `bucket_step_pinned` is unconditionally active so this is dead code, but the
    /// chain primitive requires a value to thread through inactive steps.
    static constexpr std::size_t default_value = 15u;

    template <std::size_t K, auto... /*RestPinned*/>
    using value_list_for = per_k_breakpoints_seq<K>;

    template <std::size_t /*K_pinned*/, has_bp_size QR_arg, has_bp_size TR_arg, class... Rest>
    [[nodiscard, gnu::always_inline]] static std::size_t read_field_for(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        const QR_arg& q, const TR_arg& t,
        Rest&&...) noexcept
    {
        const std::size_t max_bp = std::max(q.bp_size(), t.bp_size());
        return round_up_seq_len_log2(
            max_bp, static_cast<std::size_t>(params.kmer_size));
    }
};

} // namespace detail

// =============================================================================
// DOC0256 §5.7 / §5.8 / DOC0261 §4.6 — dispatch_chain primitive and step adapters
// =============================================================================
//
// The chain primitive and the three step adapters now live in
// `bioseq_common.templating::dispatch_chain.cppm` (DOC0261 §4.6).
// They are re-exported here so the pipeline body keeps unqualified
// names. The per-pipeline aliases below pin `Params = Minimap2Params`;
// `pipeline_dispatch_chain` is the local alias that supplies the
// `Minimap2Params` slot so chain call sites do not need to repeat it.

namespace detail {

using ::bioseq::common::templating::enum_step;
using ::bioseq::common::templating::bucket_step;
using ::bioseq::common::templating::bucket_step_pinned;
using ::bioseq::common::templating::guarded_step;

using baa_step = enum_step<algorithm_enum_values<
    ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType>>;
using ca_step  = enum_step<algorithm_enum_values<
    ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType>>;
using ma_step  = enum_step<algorithm_enum_values<
    ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>>;
using kmer_size_step    = enum_step<kmer_size_registry>;
using seq_len_log2_step = bucket_step<seq_len_log2_registry>;

/// DOC0271 Phase 1 §5.1 — per-K seq_len_log2 step. Must follow `kmer_size_step`
/// in the chain so the pinned-K's per-K breakpoint list is iterated.
using seq_len_log2_step_per_k = bucket_step_pinned<seq_len_log2_registry_per_k>;

/// Per-pipeline chain alias — pins `Params` to the only param type
/// pipe.cppm ever uses. `R` and the leaf template stay as template
/// parameters because real call sites instantiate with different
/// return types (MatchResultCollection / PipelineDescription / void).
template <class R, template <auto...> class Leaf, class... Steps>
using dispatch_chain = ::bioseq::common::templating::dispatch_chain<
    R, Leaf, ::bioseq::simd::HWY_NAMESPACE::Minimap2Params, Steps...>;

} // namespace detail

// =============================================================================
// DOC0256 §5.10 — Leaf types and traits aliases
// =============================================================================
//
// `pipeline_run_leaf<EP, QR, TR, MT, IT, L, K, BAA, CA, MA>` is the
// namespace-scope class template that bottoms out the runtime cascade.
// The Cep (Specialised vs Fallback) branch lifted from the deleted
// `dispatch_on_specialisation` lives inside the leaf body.
//
// `pipeline_run_traits<EP, QR, TR, MT, IT>::leaf<L, K, BAA, CA, MA>` is the
// alias-template wrapper exposing the leaf as an NTTP-only `template
// <auto...> class` view — the shape `dispatch_chain` requires for its
// `template <auto...> class Leaf` template-template parameter.

namespace detail {

template <EntryPoint EP_v,
          sequence_alphabet Alphabet,
          class MinimizerT, class IndexT,
          std::size_t L, std::size_t K,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA>
struct pipeline_run_leaf {
    static_assert(std::is_same_v<decltype(L), std::size_t>,
        "pipeline_run_leaf: L must be std::size_t (comes from seq_len_log2_step).");
    static_assert(std::is_same_v<decltype(K), std::size_t>,
        "pipeline_run_leaf: K must be std::size_t (comes from kmer_size_step).");
    static_assert(std::is_same_v<decltype(BAA),
        ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType>,
        "pipeline_run_leaf: BAA must be BaseAlignmentAlgorithmType (from baa_step).");
    static_assert(std::is_same_v<decltype(CA),
        ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType>,
        "pipeline_run_leaf: CA must be ChainingAlgorithmType (from ca_step).");
    static_assert(std::is_same_v<decltype(MA),
        ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>,
        "pipeline_run_leaf: MA must be MinimizerAlgorithmType (from ma_step).");

    template <class QueryMinimizers>
    BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR
    static MatchResultCollection call(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
        const QueryMinimizers&            query_minimizers,
        const IndexT&                     target_index,
        bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache,
        bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity,
        bool allow_gpu_target_index_build,
        TransientTargetIndexReleaseAuthority
            transient_target_index_owner = {})
    {
        using SpecCep     = ConstexprAlignerParams<10u, K, L>;
        #if BIOSEQ_OPTTIMIZATION_LEVEL <= 2
        using FallbackCep = ConstexprAlignerFallbackParams<63>;
        #else
        using FallbackCep = ConstexprAlignerFallbackParams<L>;
        #endif

        #if BIOSEQ_OPTTIMIZATION_LEVEL >= 5
        // Cep selection — lifted from the deleted `dispatch_on_specialisation`.
        // Both Cep instantiations are present in the binary; the runtime
        // `if` picks one.  Compile-time impact: identical to the v0 body.
        if (params.window_size == 10
            && params.kmer_size == static_cast<int32_t>(K))
        {
            return pipeline_run_impl<SpecCep, BAA, CA, MA, EP_v, Alphabet,
                                     MinimizerT, IndexT, QueryMinimizers>(
                query_seq, target_seq, query_minimizers, target_index, params,
                gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build,
                transient_target_index_owner);
        }
        #endif
        return pipeline_run_impl<FallbackCep, BAA, CA, MA, EP_v, Alphabet,
                                 MinimizerT, IndexT, QueryMinimizers>(
            query_seq, target_seq, query_minimizers, target_index, params,
            gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build,
            transient_target_index_owner);
    }
};

template <EntryPoint EP_v,
          sequence_alphabet Alphabet,
          class MinimizerT, class IndexT>
struct pipeline_run_traits {
    template <auto K, auto L, auto BAA, auto CA, auto MA>
    using leaf = pipeline_run_leaf<
        EP_v, Alphabet, MinimizerT, IndexT, L, K, BAA, CA, MA>;
};

/// Common leaf body shared by both Spec and Fallback Cep branches inside
/// `describe_leaf::call`.  Identical to the legacy `describe_pipeline_leaf`
/// from the v0 design — kept under that name so the §7.1 chain instantiation
/// can be expressed without renaming the legacy entry.  Declared before
/// `describe_leaf` so its unqualified call inside `describe_leaf::call`
/// resolves at the point of template definition.
template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          sequence_alphabet Alphabet>
[[nodiscard]] PipelineDescription describe_pipeline_leaf(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    sequence_size_t query_len, sequence_size_t target_len)
{
    using MinimizerT = typename Cep::minimizer_repr_t;
    const std::vector<MinimizerT> empty_minimizers{};
    MinimizerHashmapFragmentIndex empty_index{};
    MatchResultCollection         ignored_out{};
    const auto& sm = thread_local_scoring_matrix<Alphabet>(params);

    auto pipeline =
        make_minimap_pipeline<Cep, BAA, CA, MA, EP, Alphabet, MinimizerT,
                              MinimizerHashmapFragmentIndex>(
        query_seq, target_seq, empty_minimizers, empty_index, sm, ignored_out,
        params, query_len, target_len);
    return describe_with_params<Cep>(pipeline);
}

// Describe-side leaf — see DOC0256 §5.10.3 for the rationale on why
// EP/MA are pinned constants here instead of chain-dispatched dimensions.
template <sequence_alphabet Alphabet,
          std::size_t L, std::size_t K,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA>
struct describe_leaf {
    static_assert(std::is_same_v<decltype(L), std::size_t>,
        "describe_leaf: L must be std::size_t (from seq_len_log2_step).");
    static_assert(std::is_same_v<decltype(K), std::size_t>,
        "describe_leaf: K must be std::size_t (from kmer_size_step).");
    static_assert(std::is_same_v<decltype(BAA),
        ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType>,
        "describe_leaf: BAA must be BaseAlignmentAlgorithmType (from baa_step).");
    static_assert(std::is_same_v<decltype(CA),
        ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType>,
        "describe_leaf: CA must be ChainingAlgorithmType (from ca_step).");

    BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR
    static PipelineDescription call(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> q,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> t,
        const int32_t& query_len,
        const int32_t& target_len)
    {
        using SpecCep     = ConstexprAlignerParams<10u, K, L>;
        #if BIOSEQ_OPTTIMIZATION_LEVEL <= 2
        using FallbackCep = ConstexprAlignerFallbackParams<63>;
        #else
        using FallbackCep = ConstexprAlignerFallbackParams<L>;
        #endif

        constexpr auto EP_describe = EntryPoint::WithPrebuiltMinimizers;
        constexpr auto MA_describe = algorithm_enum_values<
            ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>::default_value;
        static_assert(MA_describe ==
            ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR,
            "describe_leaf relies on MA default = SCALAR.  Update both call "
            "sites if the MA default ever changes.");

        #if BIOSEQ_OPTTIMIZATION_LEVEL >= 5
        if (params.window_size == 10
            && params.kmer_size == static_cast<int32_t>(K))
        {
            return describe_pipeline_leaf<SpecCep, BAA, CA,
                MA_describe, EP_describe, Alphabet>(
                    q, t, params, query_len, target_len);
        }
        #endif
        return describe_pipeline_leaf<FallbackCep, BAA, CA,
            MA_describe, EP_describe, Alphabet>(
                q, t, params, query_len, target_len);
    }
};

template <sequence_alphabet Alphabet>
struct describe_traits {
    // DOC0271 Phase 1 §5.1: chain pins K before L (see pipeline_run_traits).
    template <auto K, auto L, auto BAA, auto CA>
    using leaf = describe_leaf<Alphabet, L, K, BAA, CA>;
};

} // namespace detail

// =============================================================================
// DOC0256 §4.4 / §6.1 — top-level runtime cascade entry
// =============================================================================
//
// Replaces every per-level dispatcher function (`dispatch_on_chain`,
// `dispatch_on_chain_and_mini`, `dispatch_on_algorithms`,
// `dispatch_on_specialisation`) and the hand-coded `(K, L)` ladder.  The
// body is a single `dispatch_chain<...>::run<>(...)` line over five steps
// (L, K, BAA, CA, MA) — see DOC0256 §R16.

template <EntryPoint EP,
          sequence_alphabet Alphabet,
          class MinimizerT, class IndexT,
          class QueryMinimizers = std::vector<MinimizerT>>
BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR
MatchResultCollection dispatch_pipeline(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const QueryMinimizers&            query_minimizers,
    const IndexT&                     target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false,
    TransientTargetIndexReleaseAuthority
        transient_target_index_owner = {})
{
    using Traits = detail::pipeline_run_traits<EP, Alphabet, MinimizerT, IndexT>;
    // DOC0271 Phase 1 §5.1 — K-then-L chain order so the L step (per-K) can
    // iterate only the breakpoints relevant for the pinned K.
    return detail::dispatch_chain<
        MatchResultCollection,
        Traits::template leaf,
        detail::kmer_size_step,
        detail::seq_len_log2_step_per_k,
        detail::baa_step,
        detail::ca_step,
        detail::guarded_step<detail::ma_step,
            /*active=*/(EP != EntryPoint::WithPrebuiltMinimizers)>
    >::template run<>(
        params, query_seq, target_seq, query_minimizers, target_index,
        gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build,
        transient_target_index_owner);
}

// =============================================================================
// DOC0256 §7.1 — describe_pipeline_for (EXPLAIN path)
// =============================================================================
//
// One chain instantiation over four steps (L, K, BAA, CA).  EP and MA are
// pinned constants inside `describe_leaf` (§5.10.3) — describe output is
// invariant in them.

template <sequence_alphabet Alphabet>
BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR
PipelineDescription describe_pipeline_for(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq_proto,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq_proto)
{
    const sequence_size_t query_len  = query_seq_proto.bp_size();
    const sequence_size_t target_len = target_seq_proto.bp_size();

    using Traits = detail::describe_traits<Alphabet>;
    // DOC0271 Phase 1 §5.1 — K-then-L chain order (per-K L step).
    return detail::dispatch_chain<
        PipelineDescription,
        Traits::template leaf,
        detail::kmer_size_step,
        detail::seq_len_log2_step_per_k,
        detail::baa_step,
        detail::ca_step
    >::template run<>(
        params, query_seq_proto, target_seq_proto, query_len, target_len);
}

} // namespace bioseq::simd::HWY_NAMESPACE::pipe

// =============================================================================
// 9.7 Public entry points (DOC0249 §9.7) — relocated from minimap_port.cppm so
// they can delegate to `pipe::dispatch_pipeline` without forming a circular
// import.  Live in `bioseq::simd::HWY_NAMESPACE` so callers' fully-qualified
// names (`bioseq::simd::active_target::minimap_port(...)`,
// `bioseq::simd::active_target::minimap2_port_with_index(...)`) are unchanged.
// =============================================================================

namespace bioseq::simd::HWY_NAMESPACE {

// DOC0279 §9 — relocated index name in scope for this namespace's entry points.
using bioseq::indexing::MinimizerHashmapFragmentIndex;
using bioseq::indexing::MinimizerLookupIndex;
using bioseq::indexing::MinimizerSeedIndex;

namespace pipe_entry_detail {

// Restrict fixed-allocation adoption to mmap-scale targets.  Within this route,
// a sparse result whose allocator pages were already resident retains its exact
// permit; query and ordinary small-target sketches keep the collector route.
inline constexpr std::uint64_t
    kVulkanFixedHostAdoptionTargetMinimumBp = std::uint64_t{256} << 20u;

/// Select the fixed host-ownership transaction only for a large target sketch
/// whose complete execution tuple is accepted by the source-direct device ABI.
/// Query sketching and every incompatible target intentionally remain on the
/// established collector-compatible Vulkan route.
template <class MinimizerT>
[[nodiscard]] constexpr bool
fixed_vulkan_target_host_adoption_route_selected(
        bool phase_c_gpu_index,
        const Minimap2Params& params,
        std::uint32_t alphabet_size,
        std::uint64_t target_bp) noexcept {
    return !phase_c_gpu_index
        && params.vulkan_enabled
        && params.vulkan_seed_hits_enabled
        && !params.vulkan_fused_frontend_enabled
        && !params.vulkan_gpu_index_build_enabled
        && params.minimizer_algorithm
            == MinimizerAlgorithmType::VULKAN_MINIMAP_INT
        && bioseq::indexing::gpu_source_direct_tuple_supported<MinimizerT>(
            alphabet_size,
            params.kmer_size,
            params.window_size)
        && target_bp >= kVulkanFixedHostAdoptionTargetMinimumBp;
}

/// Evidence carried with the fixed Vulkan target host allocation.  The host
/// allocation has exactly one accounting authority: either its fully-touched
/// bytes were reconciled into process footprint and the permit was released,
/// or an exact permit remains attached to the allocation.  Empty output is the
/// footprint-authoritative zero-byte case.
template <class MinimizerT>
struct VulkanSketchHostAdoptionReceipt {
    const MinimizerT* source_data = nullptr;
    const MinimizerT* adopted_data = nullptr;
    std::size_t adopted_size = 0u;
    std::size_t adopted_capacity = 0u;
    std::size_t source_capacity_after_adoption = 0u;
    std::uint64_t host_capacity_bytes = 0u;
    std::uint64_t retained_host_permit_bytes = 0u;
    std::uint64_t host_footprint_reflected_delta_bytes = 0u;
    std::uint64_t device_allocation_bytes_released = 0u;
    bool host_permit_released_to_process_footprint = false;
    bool device_owner_present_before_adoption = false;
    bool device_owner_unique_before_adoption = false;
    bool device_released_before_return = false;
    bool arena_trimmed_before_adoption = false;
    bioseq::gpu::vulkan::VulkanSketchDispatchToken dispatch_token{};

    [[nodiscard]] bool zero_copy_adoption_accepted() const noexcept {
        const std::uint64_t expected_capacity_bytes =
            bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(adopted_capacity),
                static_cast<std::uint64_t>(sizeof(MinimizerT)));
        const bool allocation_identity_accepted = adopted_size == 0u
            ? adopted_capacity == 0u && host_capacity_bytes == 0u
            : source_data == adopted_data
                && adopted_capacity == adopted_size
                && host_capacity_bytes == expected_capacity_bytes;
        const bool footprint_authority =
            host_permit_released_to_process_footprint
            && retained_host_permit_bytes == 0u
            && (host_capacity_bytes == 0u
                || host_footprint_reflected_delta_bytes
                    >= host_capacity_bytes);
        const bool retained_permit_authority =
            !host_permit_released_to_process_footprint
            && host_capacity_bytes != 0u
            && retained_host_permit_bytes == host_capacity_bytes;
        return allocation_identity_accepted
            && expected_capacity_bytes
                != (std::numeric_limits<std::uint64_t>::max)()
            && source_capacity_after_adoption == 0u
            && footprint_authority != retained_permit_authority
            && device_owner_present_before_adoption
            && device_owner_unique_before_adoption
            && device_released_before_return
            && arena_trimmed_before_adoption
            && dispatch_token.accepted();
    }
};

/// Return-by-value lifetime owner for an adopted fixed Vulkan target sketch.
/// The permit is declared before the vector so normal reverse destruction frees
/// the allocation before returning its reservation.  Move assignment is
/// intentionally unavailable: memberwise assignment would release an existing
/// destination permit before freeing its destination vector.
template <class MinimizerT>
struct FixedVulkanTargetHostOwner {
    std::optional<bioseq::parallelism::MemoryPermit> host_permit;
    std::vector<MinimizerT> minimizers;
    VulkanSketchHostAdoptionReceipt<MinimizerT> adoption{};

    FixedVulkanTargetHostOwner() = default;
    FixedVulkanTargetHostOwner(const FixedVulkanTargetHostOwner&) = delete;
    FixedVulkanTargetHostOwner& operator=(
        const FixedVulkanTargetHostOwner&) = delete;
    FixedVulkanTargetHostOwner& operator=(
        FixedVulkanTargetHostOwner&&) = delete;

    FixedVulkanTargetHostOwner(
            FixedVulkanTargetHostOwner&& other) noexcept {
        // Swap the allocation while the source permit is still live, then move
        // its authority.  The destination is newly constructed and empty.
        minimizers.swap(other.minimizers);
        adoption = other.adoption;
        other.adoption = {};
        if (other.host_permit.has_value()) {
            host_permit.emplace(std::move(*other.host_permit));
        }
        other.host_permit.reset();
    }

    [[nodiscard]] bool ownership_authority_accepted() const noexcept {
        const bool valid_permit_present = host_permit.has_value()
            && static_cast<bool>(*host_permit);
        const bool footprint_authority =
            adoption.host_permit_released_to_process_footprint
            && !host_permit.has_value();
        const bool retained_permit_authority =
            !adoption.host_permit_released_to_process_footprint
            && valid_permit_present
            && host_permit->bytes() == adoption.host_capacity_bytes
            && host_permit->bytes()
                == adoption.retained_host_permit_bytes;
        const bool allocation_matches_receipt =
            minimizers.size() == adoption.adopted_size
            && minimizers.capacity() == adoption.adopted_capacity
            && (minimizers.empty()
                || minimizers.data() == adoption.adopted_data);
        return adoption.zero_copy_adoption_accepted()
            && allocation_matches_receipt
            && footprint_authority != retained_permit_authority;
    }

    void release_consumed_host_permit() {
        if (minimizers.capacity() != 0u) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "fixed Vulkan host permit release preceded vector "
                "deallocation"};
        }
        host_permit.reset();
    }
};

template <class MinimizerT>
void release_fixed_vulkan_source_after_resident_publication(
    FixedVulkanTargetHostOwner<MinimizerT>& source,
    pipe::DirectResidentOperationLedger& ledger,
    bool resident_published)
{
    const std::uint64_t released_bytes =
        bioseq::parallelism::saturating_mul(
            source.minimizers.capacity(), sizeof(MinimizerT));
    {
        std::vector<MinimizerT> empty;
        source.minimizers.swap(empty);
    }
    if (source.host_permit.has_value()) {
        if (source.minimizers.capacity() != 0u) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "source-direct host permit release preceded source storage"};
        }
        source.host_permit.reset();
    }
    source.adoption = {};
    ledger.source_released_bytes = released_bytes;
    ledger.source_releases = released_bytes != 0u ? 1u : 0u;
    ledger.source_release_completed = true;
    ledger.source_released_after_publication = resident_published;
}

/// Exact accounting returned by the fixed Vulkan host-vector -> CPU index
/// handoff.  Keeping the transaction in one helper lets tests exercise the
/// same reserve/build/validate path used by `minimap2_port` instead of merely
/// reproducing its arithmetic.
struct VulkanTargetIndexHandoffReceipt {
    std::uint64_t admitted_position_headroom_bytes = 0u;
    std::uint64_t admitted_table_headroom_bytes = 0u;
    std::uint64_t admitted_build_bytes = 0u;
    std::uint64_t retained_position_bytes = 0u;
    std::uint64_t retained_table_bytes = 0u;
    std::uint64_t retained_index_bytes = 0u;
    std::uint64_t retained_index_permit_bytes = 0u;
    std::uint64_t transferred_position_footprint_bytes = 0u;
    std::uint64_t source_host_capacity_bytes_before_build = 0u;
    std::uint64_t source_footprint_reflected_bytes_before_build = 0u;
    std::uint64_t source_minimizer_count_before_build = 0u;
    std::uint64_t retained_position_count = 0u;
    std::uint64_t retained_position_initialized_bytes = 0u;
    std::uint64_t source_host_permit_bytes_before_build = 0u;
    std::uint64_t reserved_bytes_before_index_admission = 0u;
    std::uint64_t reserved_bytes_after_index_admission = 0u;
    bool source_host_permit_retained_during_build = false;
    bool source_footprint_authority_transferred_to_positions = false;
    bool every_retained_position_initialized = false;
    bool source_capacity_released = false;
    bool source_host_permit_cleared_after_consumption = false;
    bool source_adoption_cleared_after_consumption = false;

    [[nodiscard]] bool accepted() const noexcept {
        return source_capacity_released
            && source_host_permit_cleared_after_consumption
            && source_adoption_cleared_after_consumption
            && retained_position_bytes
                <= admitted_position_headroom_bytes
            && retained_table_bytes <= admitted_table_headroom_bytes
            && retained_index_bytes
                == bioseq::parallelism::saturating_add(
                    retained_position_bytes, retained_table_bytes)
            && retained_index_bytes
                == bioseq::parallelism::saturating_add(
                    retained_index_permit_bytes,
                    transferred_position_footprint_bytes)
            && transferred_position_footprint_bytes
                <= retained_position_bytes
            && (transferred_position_footprint_bytes == 0u
                || transferred_position_footprint_bytes
                    == retained_position_bytes)
            && every_retained_position_initialized
            && retained_position_count
                == source_minimizer_count_before_build
            && retained_position_initialized_bytes
                == retained_position_bytes
            && retained_position_initialized_bytes
                == bioseq::parallelism::saturating_mul(
                    retained_position_count,
                    static_cast<std::uint64_t>(sizeof(std::uint64_t)))
            && (!source_footprint_authority_transferred_to_positions
                ? transferred_position_footprint_bytes == 0u
                : transferred_position_footprint_bytes != 0u
                    && !source_host_permit_retained_during_build
                    && source_host_permit_bytes_before_build == 0u
                    && source_host_capacity_bytes_before_build
                        == transferred_position_footprint_bytes
                    && source_footprint_reflected_bytes_before_build
                        >= source_host_capacity_bytes_before_build);
    }
};

/// Owning return value for the strict fixed-Vulkan target-index handoff.  The
/// index permit precedes the index so reverse destruction releases table and
/// position storage first.  Move assignment is deleted to rule out releasing a
/// destination permit before its previous index storage is deallocated.
struct VulkanAdmittedTargetIndex {
    std::optional<bioseq::parallelism::MemoryPermit> index_permit;
    MinimizerHashmapFragmentIndex index;
    VulkanTargetIndexHandoffReceipt receipt{};

    VulkanAdmittedTargetIndex() = default;
    VulkanAdmittedTargetIndex(const VulkanAdmittedTargetIndex&) = delete;
    VulkanAdmittedTargetIndex& operator=(
        const VulkanAdmittedTargetIndex&) = delete;
    VulkanAdmittedTargetIndex& operator=(VulkanAdmittedTargetIndex&&) = delete;

    VulkanAdmittedTargetIndex(VulkanAdmittedTargetIndex&& other) noexcept
        : index(std::move(other.index)),
          receipt(other.receipt) {
        // Move storage while the source reservation remains authoritative,
        // then transfer the reservation into this new empty owner.
        if (other.index_permit.has_value()) {
            index_permit.emplace(std::move(*other.index_permit));
        }
        other.index_permit.reset();
        other.receipt = {};
    }

    [[nodiscard]] bool accepted() const noexcept {
        return receipt.accepted()
            && index_permit.has_value()
            && static_cast<bool>(*index_permit)
            && index_permit->bytes()
                == receipt.retained_index_permit_bytes;
    }

    void release_storage_before_permit() noexcept {
        index.release_storage();
        index_permit.reset();
    }
};

template <class MinimizerT>
[[nodiscard]] VulkanAdmittedTargetIndex
admit_and_build_fixed_vulkan_target_index(
    FixedVulkanTargetHostOwner<MinimizerT> target_host,
    sequence_size_t target_len)
{
    if (!target_host.ownership_authority_accepted()) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError{
            "Vulkan zero-copy target-index handoff lacks host ownership "
            "authority"};
    }
    const bool source_host_permit_retained_during_build =
        target_host.host_permit.has_value();
    const bool source_footprint_authority_before_build =
        target_host.adoption.host_permit_released_to_process_footprint
        && !target_host.host_permit.has_value();
    const std::uint64_t source_host_capacity_bytes_before_build =
        target_host.adoption.host_capacity_bytes;
    const std::uint64_t source_footprint_reflected_bytes_before_build =
        target_host.adoption.host_footprint_reflected_delta_bytes;
    const std::uint64_t source_minimizer_count_before_build =
        static_cast<std::uint64_t>(target_host.minimizers.size());
    const std::uint64_t source_host_permit_bytes_before_build =
        source_host_permit_retained_during_build
            ? target_host.host_permit->bytes()
            : 0u;
    const std::uint64_t position_payload_bytes =
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(target_host.minimizers.size()),
            static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    // Empty sketches retain neither positions nor a table.  Do not turn the
    // generic allocator slop allowance into a fictitious 128 KiB reservation.
    const std::uint64_t admitted_position_headroom_bytes =
        position_payload_bytes == 0u
            ? 0u
            : bioseq::gpu::vulkan::
                vulkan_exact_host_allocation_admission_bytes(
                    position_payload_bytes);
    const bioseq::indexing::MinimizerHashmapTableAllocationPlan table_plan =
        bioseq::indexing::minimizer_hashmap_table_allocation_plan(
            static_cast<std::uint64_t>(target_host.minimizers.size()));
    const std::uint64_t admitted_table_headroom_bytes = !table_plan.valid
        ? (std::numeric_limits<std::uint64_t>::max)()
        : table_plan.allocation_bytes == 0u
            ? 0u
            : bioseq::gpu::vulkan::
                vulkan_exact_host_allocation_admission_bytes(
                    table_plan.allocation_bytes);
    const std::uint64_t build_peak_bytes =
        bioseq::parallelism::saturating_add(
            admitted_position_headroom_bytes,
            admitted_table_headroom_bytes);
    if (position_payload_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || admitted_position_headroom_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || admitted_table_headroom_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || build_peak_bytes
            == (std::numeric_limits<std::uint64_t>::max)()) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError{
            "Vulkan zero-copy target-index handoff has invalid "
            "capacity accounting"};
    }
    VulkanAdmittedTargetIndex admitted_index;
    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t reserved_bytes_before_index_admission =
        admission.reserved_bytes();
    if (source_host_permit_retained_during_build) {
        // This deliberately counts the source reservation even if some reused
        // allocator pages are visible in RSS.  The conservative overlap can
        // deny a medium sparse result, but it never over-admits the real
        // host-vector + positions + one-shot FOA-table build peak.
        bioseq_log<DBG>(
            "[vulkan sketch] strict target-index handoff keeps sparse host "
            "permit through build: host={} index_peak={}",
            source_host_permit_bytes_before_build,
            build_peak_bytes);
    }
    bioseq::parallelism::MemoryAdmissionFailure failure{};
    auto permit = admission.try_reserve(build_peak_bytes, &failure);
    if (!permit.has_value()) {
        bioseq_log<ERR>(
            "[vulkan sketch] strict target-index handoff admission "
            "denied: peak={} positions={} table={} requested={} "
            "reserved={} allocatable_after_reservations={}",
            build_peak_bytes,
            admitted_position_headroom_bytes,
            admitted_table_headroom_bytes,
            failure.requested_bytes,
            failure.reserved_bytes,
            failure.allocatable_after_reservations_bytes);
        throw bioseq::gpu::vulkan::VulkanRuntimeError{
            "Vulkan zero-copy target-index handoff admission denied"};
    }
    admitted_index.index_permit.emplace(std::move(*permit));
    const std::uint64_t reserved_bytes_after_index_admission =
        admission.reserved_bytes();
    admitted_index.index.build_consuming_full_table(
        std::move(target_host.minimizers), target_len);
    const std::uint64_t retained_position_bytes =
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(
                admitted_index.index.positions.capacity()),
            static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    const std::uint64_t retained_table_bytes =
        admitted_index.index.table_allocation_bytes();
    const std::uint64_t retained_position_count =
        static_cast<std::uint64_t>(admitted_index.index.positions.size());
    const std::uint64_t retained_position_initialized_bytes =
        bioseq::parallelism::saturating_mul(
            retained_position_count,
            static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    const std::uint64_t retained_index_bytes =
        bioseq::parallelism::saturating_add(
            retained_position_bytes, retained_table_bytes);
    if (target_host.minimizers.capacity() != 0u
        || retained_position_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || retained_position_initialized_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || retained_position_bytes > admitted_position_headroom_bytes
        || retained_position_count != source_minimizer_count_before_build
        || admitted_index.index.table_peak_allocation_bytes()
            > admitted_table_headroom_bytes
        || retained_table_bytes != table_plan.allocation_bytes
        || retained_index_bytes
            == (std::numeric_limits<std::uint64_t>::max)()) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError{
            "Vulkan zero-copy target-index build exceeded admitted "
            "host capacity"};
    }
    // The source allocation is gone now and only now may its retained permit
    // be returned.  On every earlier throw the by-value host owner unwinds its
    // vector before its permit.
    target_host.release_consumed_host_permit();
    const bool source_host_permit_cleared_after_consumption =
        !target_host.host_permit.has_value();
    // Exact replacement accounting is deliberately all-or-nothing.  The
    // footprint route may transfer its one-use authority only after every
    // position is initialized and the source allocation is gone, and only
    // when the replacement has exactly the same nonzero byte extent.  Any
    // mismatch discards the old authority and retains the full index permit.
    const bool exact_position_footprint_replacement =
        source_footprint_authority_before_build
        && source_host_capacity_bytes_before_build != 0u
        && source_footprint_reflected_bytes_before_build
            >= source_host_capacity_bytes_before_build
        && retained_position_count == source_minimizer_count_before_build
        && retained_position_initialized_bytes == retained_position_bytes
        && target_host.minimizers.capacity() == 0u
        && source_host_capacity_bytes_before_build
            == retained_position_bytes;
    const std::uint64_t transferred_position_footprint_bytes =
        exact_position_footprint_replacement
            ? retained_position_bytes
            : 0u;
    const std::uint64_t retained_index_permit_bytes =
        retained_index_bytes - transferred_position_footprint_bytes;
    bioseq_log<INF>(
        "[vulkan sketch] strict target-index replacement authority: "
        "source_authority={} source_capacity={} source_reflected={} "
        "source_permit={} source_count={} position_count={} "
        "position_initialized={} position_capacity={} exact_replacement={} "
        "transferred_positions={} retained_permit={} retained_total={}",
        source_footprint_authority_before_build
            ? "process_footprint"
            : source_host_permit_retained_during_build
                ? "retained_permit"
                : "invalid",
        source_host_capacity_bytes_before_build,
        source_footprint_reflected_bytes_before_build,
        source_host_permit_bytes_before_build,
        source_minimizer_count_before_build,
        retained_position_count,
        retained_position_initialized_bytes,
        retained_position_bytes,
        exact_position_footprint_replacement,
        transferred_position_footprint_bytes,
        retained_index_permit_bytes,
        retained_index_bytes);
    target_host.adoption = {};
    const bool source_adoption_cleared_after_consumption =
        target_host.adoption.adopted_size == 0u
        && target_host.adoption.host_capacity_bytes == 0u
        && !target_host.adoption.
            host_permit_released_to_process_footprint;
    admitted_index.index_permit->shrink_to(retained_index_permit_bytes);
    admitted_index.receipt = VulkanTargetIndexHandoffReceipt{
        .admitted_position_headroom_bytes =
            admitted_position_headroom_bytes,
        .admitted_table_headroom_bytes = admitted_table_headroom_bytes,
        .admitted_build_bytes = build_peak_bytes,
        .retained_position_bytes = retained_position_bytes,
        .retained_table_bytes = retained_table_bytes,
        .retained_index_bytes = retained_index_bytes,
        .retained_index_permit_bytes = retained_index_permit_bytes,
        .transferred_position_footprint_bytes =
            transferred_position_footprint_bytes,
        .source_host_capacity_bytes_before_build =
            source_host_capacity_bytes_before_build,
        .source_footprint_reflected_bytes_before_build =
            source_footprint_reflected_bytes_before_build,
        .source_minimizer_count_before_build =
            source_minimizer_count_before_build,
        .retained_position_count = retained_position_count,
        .retained_position_initialized_bytes =
            retained_position_initialized_bytes,
        .source_host_permit_bytes_before_build =
            source_host_permit_bytes_before_build,
        .reserved_bytes_before_index_admission =
            reserved_bytes_before_index_admission,
        .reserved_bytes_after_index_admission =
            reserved_bytes_after_index_admission,
        .source_host_permit_retained_during_build =
            source_host_permit_retained_during_build,
        .source_footprint_authority_transferred_to_positions =
            exact_position_footprint_replacement,
        .every_retained_position_initialized =
            retained_position_count == source_minimizer_count_before_build
            && retained_position_initialized_bytes
                == retained_position_bytes,
        .source_capacity_released =
            target_host.minimizers.capacity() == 0u,
        .source_host_permit_cleared_after_consumption =
            source_host_permit_cleared_after_consumption,
        .source_adoption_cleared_after_consumption =
            source_adoption_cleared_after_consumption,
    };
    if (!admitted_index.accepted()) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError{
            "Vulkan zero-copy target-index owning receipt was rejected"};
    }
    return admitted_index;
}

} // namespace pipe_entry_detail

/**
 * @brief Run the minimap2 pipeline against a pre-built target index using a
 * caller-supplied query minimiser vector (DOC0249 §9.7).
 *
 * Single-query convenience wrapper that hands off to
 * `pipe::dispatch_pipeline<EP::WithPrebuiltMinimizers>(...)`.  All algorithm-
 * choice and bucket dispatch happens inside the pipe cascade — no
 * `(BAA, CA)` switch at this layer.
 */
template <class MinimizerT,
          sequence_alphabet Alphabet,
          ::bioseq::indexing::MinimizerSeedIndex IndexT>
[[nodiscard]] MatchResultCollection minimap2_port_with_index(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const std::vector<MinimizerT>& query_minimizers,
    const IndexT& prebuilt_target_index,
    const Minimap2Params& params = Minimap2Params{},
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap2_port_with_index");
    auto _scope = bioseq::instr::region(_desc);
    return ::bioseq::simd::HWY_NAMESPACE::pipe::dispatch_pipeline<
        ::bioseq::simd::HWY_NAMESPACE::pipe::EntryPoint::WithPrebuiltMinimizers,
            Alphabet, MinimizerT, IndexT>(
            query_seq, target_seq, query_minimizers, prebuilt_target_index, params,
            gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build);
}

template <class MinimizerT,
          sequence_alphabet Alphabet,
          ::bioseq::indexing::MinimizerSeedIndex IndexT>
[[nodiscard]] MatchResultCollection minimap2_port_with_index(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const ::bioseq::simd::HWY_NAMESPACE::pipe::MinimizerBatchView<MinimizerT>& query_minimizers,
    const IndexT& prebuilt_target_index,
    const Minimap2Params& params = Minimap2Params{},
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap2_port_with_index");
    auto _scope = bioseq::instr::region(_desc);
    return ::bioseq::simd::HWY_NAMESPACE::pipe::dispatch_pipeline<
        ::bioseq::simd::HWY_NAMESPACE::pipe::EntryPoint::WithPrebuiltMinimizers,
            Alphabet, MinimizerT, IndexT>(
            query_seq, target_seq, query_minimizers, prebuilt_target_index, params,
            gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build);
}

/**
 * @brief Run the minimap2 pipeline against pre-built target minimizers
 * (DOC0249 §9.7 / legacy minimap2_port shape).
 *
 * Builds a TargetIndex from `target_minimizers` and delegates to
 * `minimap2_port_with_index`.
 */
template <class MinimizerT,
          sequence_alphabet Alphabet>
[[nodiscard]] MatchResultCollection minimap2_port(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const std::vector<MinimizerT>& query_minimizers,
    std::vector<MinimizerT> target_minimizers,
    const Minimap2Params& params = Minimap2Params{})
{
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap2_port");
    auto _scope = bioseq::instr::region(_desc);
    const sequence_size_t target_len = target_seq.bp_size();
    MinimizerHashmapFragmentIndex target_index;
    // DOC0310 §16 — for the Phase C leaf with the GPU index build active, leave
    // the CPU index empty; `pipeline_run_impl_batch` must build it on the
    // device. This is the line that removes the XXL_HIFI
    // `target_index_build` bottleneck from the GPU row.
    const bool phase_c_gpu_index =
        ::bioseq::simd::HWY_NAMESPACE::pipe::phase_c_index_build_active(params);
    if (!phase_c_gpu_index) {
        target_index.build_consuming(
            std::move(target_minimizers), target_len);
    } else {
        std::vector<MinimizerT>{}.swap(target_minimizers);
    }
    static const auto* _with_index_desc =
        bioseq::instr::register_region_static("minimap2_port_with_index");
    auto _with_index_scope = bioseq::instr::region(_with_index_desc);
    return ::bioseq::simd::HWY_NAMESPACE::pipe::dispatch_pipeline<
        ::bioseq::simd::HWY_NAMESPACE::pipe::EntryPoint::WithPrebuiltMinimizers,
            Alphabet, MinimizerT, MinimizerHashmapFragmentIndex>(
                query_seq, target_seq, query_minimizers, target_index, params,
                nullptr, {}, phase_c_gpu_index,
                ::bioseq::simd::HWY_NAMESPACE::pipe::
                    TransientTargetIndexReleaseAuthority{
                        .index = &target_index});
}

namespace pipe_entry_detail {

/// Fixed Vulkan target handoff for the selected query-independent source-direct
/// sorted-dense resident.  The adopted source is radix-sorted in place, counted
/// exactly, streamed into final mapped buffers, published under an
/// operation-unique key, and only then released.  No CPU FOA table or CPU
/// positions vector is constructed on this route.
template <class MinimizerT,
          sequence_alphabet Alphabet>
[[nodiscard]] MatchResultCollection
minimap2_port_with_fixed_vulkan_target_handoff(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const std::vector<MinimizerT>& query_minimizers,
    FixedVulkanTargetHostOwner<MinimizerT> target_host,
    const Minimap2Params& params)
{
    pipe::DirectResidentOperationLedger ledger{};
    ledger.operation_identity = bioseq::indexing::
        next_gpu_source_direct_operation_identity();
    pipe::DirectResidentReceiptGuard receipt_guard{&ledger};
    ledger.source_capacity_bytes =
        target_host.adoption.host_capacity_bytes;
    ledger.source_reflected_bytes =
        target_host.adoption.host_footprint_reflected_delta_bytes;
    ledger.source_retained_permit_bytes =
        target_host.host_permit.has_value()
            ? target_host.host_permit->bytes() : 0u;
    ledger.source_authority = target_host.minimizers.empty()
        ? pipe::SourceDirectHostAuthority::canonical_empty
        : target_host.adoption.host_permit_released_to_process_footprint
            ? pipe::SourceDirectHostAuthority::
                reflected_process_footprint
            : pipe::SourceDirectHostAuthority::retained_exact_permit;

    const sequence_size_t target_len = target_seq.bp_size();

    std::optional<bioseq::indexing::GpuMinimizerIndexCache>
        operation_gpu_cache;
    std::shared_ptr<bioseq::indexing::GpuResidentMinimizerIndex>
        construction_handle;
    MinimizerHashmapFragmentIndex empty_target_index;
    empty_target_index.target_len = target_len;
    bioseq::indexing::GpuIndexCacheIdentity cache_identity{};
    bioseq::indexing::GpuMinimizerIndexCacheKey cache_key{};

    try {
        ledger.phase_c_checked = true;
        ledger.phase_c_compatible =
            !::bioseq::simd::HWY_NAMESPACE::pipe::
                phase_c_index_build_active(params)
            && params.vulkan_enabled
            && params.vulkan_seed_hits_enabled
            && !params.vulkan_fused_frontend_enabled
            && !params.vulkan_gpu_index_build_enabled
            && params.minimizer_algorithm
                == MinimizerAlgorithmType::VULKAN_MINIMAP_INT;
        if (!ledger.phase_c_compatible) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "fixed Vulkan host target handoff has an ineligible direct tuple"};
        }

        ledger.source_authority_checked = true;
        ledger.source_authority_accepted =
            target_host.ownership_authority_accepted();
        if (!ledger.source_authority_accepted) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "source-direct target handoff lacks source authority"};
        }

        ledger.runtime_checked = true;
        auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
        ledger.runtime_available = true;
        ledger.hardware_checked = true;
        ledger.hardware_available =
            bioseq::gpu::vulkan::hardware_device_available(runtime);
        if (!ledger.hardware_available) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "source-direct target handoff has no Vulkan device"};
        }

        static const auto* _desc =
            bioseq::instr::register_region_static("minimap2_port");
        auto _scope = bioseq::instr::region(_desc);
        MinimizerHashmapFragmentIndex::sort_source_by_hash_in_place(
            target_host.minimizers);
        ledger.sorted_shape_checked = true;
        const auto inspected = bioseq::indexing::
            inspect_presorted_minimizer_source<MinimizerT>(
                std::span<const MinimizerT>{
                    target_host.minimizers.data(),
                    target_host.minimizers.size()});
        if (!inspected) {
            ledger.sorted_shape_accepted = false;
            ledger.log_planning("reject", "sorted_source_shape_invalid");
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "source-direct sorted source shape is invalid"};
        }
        ledger.sorted_shape_accepted = true;
        ledger.minimizer_count = inspected->minimizer_count;
        ledger.distinct_count = inspected->distinct_count;

        using TargetAlphabet = Alphabet;
        ledger.identity_checked = true;
        cache_identity = bioseq::indexing::
            make_gpu_source_direct_sorted_dense_identity<MinimizerT>(
                runtime,
                ledger.operation_identity,
                ledger.minimizer_count,
                ledger.distinct_count,
                static_cast<std::uint32_t>(
                    seqan3::alphabet_size<TargetAlphabet>),
                static_cast<std::uint32_t>(params.kmer_size),
                static_cast<std::uint32_t>(params.window_size));
        ledger.identity_accepted = bioseq::indexing::
            gpu_source_direct_identity_well_formed(cache_identity);
        if (!ledger.identity_accepted) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "source-direct cache identity is ineligible"};
        }
        cache_key = bioseq::indexing::make_gpu_index_cache_key(
            empty_target_index, cache_identity);

        ledger.u32_shape_checked = true;
        ledger.u32_shape_accepted =
            ledger.minimizer_count
                <= (std::numeric_limits<std::uint32_t>::max)()
            && ledger.distinct_count
                <= (std::numeric_limits<std::uint32_t>::max)()
            && ((ledger.minimizer_count == 0u)
                == (ledger.distinct_count == 0u));
        if (!ledger.u32_shape_accepted) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "source-direct resident shape exceeds the device ABI"};
        }

        ledger.storage_ranges_checked = true;
        const auto preflight = bioseq::indexing::
            plan_gpu_source_direct_sorted_dense(
                runtime,
                ledger.minimizer_count,
                ledger.distinct_count);
        ledger.max_storage_buffer_range =
            runtime.features().max_storage_buffer_range;
        if (!preflight) {
            ledger.control_range_accepted = false;
            ledger.position_range_accepted = false;
            ledger.log_planning("reject", "shape_or_storage_range_rejected");
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "source-direct resident preflight rejected"};
        }
        ledger.control_range_accepted = true;
        ledger.position_range_accepted = true;
        ledger.canonical_empty = preflight->canonical_empty;
        ledger.logical_control_bytes = preflight->control_bytes;
        ledger.logical_position_bytes = preflight->position_bytes;
        ledger.logical_resident_bytes =
            preflight->logical_resident_bytes;
        ledger.control_requirement_bytes =
            preflight->control_allocation_requirement_bytes;
        ledger.position_requirement_bytes =
            preflight->position_allocation_requirement_bytes;
        ledger.control_allowance_bytes =
            preflight->control_allocation_allowance_bytes;
        ledger.position_allowance_bytes =
            preflight->position_allocation_allowance_bytes;
        ledger.conservative_allowance_bytes = preflight->allowance_bytes;
        ledger.conservative_request_bytes =
            preflight->conservative_request_bytes;
        ledger.mapping_plan_checked = true;
        ledger.mapping_plan_accepted = true;
        const auto snapshot = bioseq::parallelism::
            MemoryAdmissionController::instance().diagnostic_snapshot();
        ledger.admission_snapshot_taken = true;
        ledger.reserved_before_bytes = snapshot.reserved_bytes;
        ledger.allocatable_before_bytes =
            snapshot.allocatable_after_reservations_bytes;
        if (ledger.allocatable_before_bytes
            >= ledger.conservative_request_bytes) {
            ledger.margin_bytes = ledger.allocatable_before_bytes
                - ledger.conservative_request_bytes;
        } else {
            ledger.shortfall_bytes = ledger.conservative_request_bytes
                - ledger.allocatable_before_bytes;
        }
        ledger.planning_accepted = ledger.shortfall_bytes == 0u;
        ledger.log_planning(
            ledger.planning_accepted ? "proceed" : "reject",
            ledger.planning_accepted
                ? "exact_shape_and_range_preflight_passed"
                : "resident_memory_shortfall");
        if (!ledger.planning_accepted) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "source-direct resident planning admission rejected"};
        }
        ledger.route_committed = true;

        if (!ledger.canonical_empty) {
            ledger.materialization_started = true;
            bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
            auto resident_permit = bioseq::parallelism::
                MemoryAdmissionController::instance().try_reserve(
                    ledger.conservative_request_bytes,
                    &admission_failure);
            if (!resident_permit) {
                throw bioseq::gpu::vulkan::VulkanRuntimeError{
                    "source-direct resident admission failed"};
            }
            auto materialized = bioseq::indexing::
                materialize_sorted_dense_from_presorted_minimizers(
                    runtime,
                    std::span<const MinimizerT>{
                        target_host.minimizers.data(),
                    target_host.minimizers.size()},
                    target_len,
                    *preflight,
                    cache_key,
                    *resident_permit);
            if (!materialized) {
                throw bioseq::gpu::vulkan::VulkanRuntimeError{
                    "source-direct resident materialization failed"};
            }
            ledger.materialization = materialized->proof;
            ledger.materialization_completed = true;
            construction_handle = std::move(materialized->resident);
            operation_gpu_cache.emplace(
                ledger.materialization.actual_allocation_bytes);
            auto inserted = operation_gpu_cache->insert_built(
                cache_key, construction_handle);
            if (!inserted) {
                throw bioseq::gpu::vulkan::VulkanRuntimeError{
                    "source-direct resident preinsert failed"};
            }
            ledger.cache_preinsertions = 1u;
            ledger.resident_published = true;
            construction_handle.reset();
        }

        release_fixed_vulkan_source_after_resident_publication(
            target_host, ledger, ledger.resident_published);

        static const auto* _with_index_desc =
            bioseq::instr::register_region_static(
                "minimap2_port_with_index");
        auto _with_index_scope = bioseq::instr::region(_with_index_desc);
        auto result = ::bioseq::simd::HWY_NAMESPACE::pipe::dispatch_pipeline<
            ::bioseq::simd::HWY_NAMESPACE::pipe::
                EntryPoint::WithPrebuiltMinimizers,
                Alphabet, MinimizerT, MinimizerHashmapFragmentIndex>(
                    query_seq,
                    target_seq,
                    query_minimizers,
                    empty_target_index,
                    params,
                    operation_gpu_cache
                        ? &*operation_gpu_cache : nullptr,
                    cache_identity,
                    /*allow_gpu_target_index_build=*/false,
                    ::bioseq::simd::HWY_NAMESPACE::pipe::
                        TransientTargetIndexReleaseAuthority{
                            .operation_owned_gpu_cache =
                                operation_gpu_cache
                                    ? &*operation_gpu_cache : nullptr,
                            .direct_ledger = &ledger});
        if (ledger.terminal_receipts != 1u || !ledger.terminal_success) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError{
                "source-direct operation terminal receipt rejected"};
        }
        PartialVulkanExecutionProof operation_proof{};
        operation_proof.gpu_index_source_direct_operations = 1u;
        operation_proof.gpu_index_source_direct_terminal_successes = 1u;
        operation_proof.gpu_index_source_direct_empty_bypasses =
            ledger.empty_bypass ? 1u : 0u;
        operation_proof.gpu_index_source_direct_zero_query_bypasses =
            ledger.zero_query_bypasses;
        operation_proof.gpu_index_source_direct_planning_receipts =
            ledger.planning_receipts;
        operation_proof.gpu_index_source_direct_terminal_receipts =
            ledger.terminal_receipts;
        operation_proof.gpu_index_source_direct_materializations =
            ledger.materialization_completed ? 1u : 0u;
        operation_proof.gpu_index_source_direct_resident_publications =
            ledger.resident_published ? 1u : 0u;
        operation_proof.gpu_index_source_direct_logical_control_bytes =
            ledger.logical_control_bytes;
        operation_proof.gpu_index_source_direct_logical_position_bytes =
            ledger.logical_position_bytes;
        operation_proof.gpu_index_source_direct_control_requirement_bytes =
            ledger.control_requirement_bytes;
        operation_proof.gpu_index_source_direct_position_requirement_bytes =
            ledger.position_requirement_bytes;
        operation_proof.gpu_index_source_direct_allocation_request_bytes =
            ledger.conservative_request_bytes;
        operation_proof.gpu_index_source_direct_actual_control_bytes =
            ledger.materialization.actual_slot_allocation_bytes;
        operation_proof.gpu_index_source_direct_actual_position_bytes =
            ledger.materialization.actual_position_allocation_bytes;
        operation_proof.gpu_index_source_direct_source_releases =
            ledger.source_releases;
        operation_proof.gpu_index_source_direct_source_released_bytes =
            ledger.source_released_bytes;
        operation_proof.gpu_index_source_direct_checksum_matches =
            ledger.materialization.source_checksum != 0u
                && ledger.materialization.source_checksum
                    == ledger.materialization.mapped_checksum
            ? 1u : 0u;
        operation_proof.gpu_index_source_direct_source_traversals =
            ledger.materialization.complete_source_traversals;
        operation_proof.gpu_index_source_direct_mapped_traversals =
            ledger.materialization.complete_mapped_traversals;
        operation_proof.gpu_index_source_direct_reflected_authorities =
            ledger.materialization.authority
                == bioseq::indexing::GpuResidentAccountingAuthority::
                    reflected_process_footprint
            ? 1u : 0u;
        operation_proof.gpu_index_source_direct_retained_authorities =
            ledger.materialization.authority
                == bioseq::indexing::GpuResidentAccountingAuthority::
                    retained_exact_permit
            ? 1u : 0u;
        operation_proof.
            gpu_index_source_direct_retained_resident_permit_bytes =
                ledger.materialization.retained_permit_bytes;
        operation_proof.gpu_index_source_direct_cache_preinsertions =
            ledger.cache_preinsertions;
        operation_proof.gpu_index_source_direct_expected_seed_consumers =
            ledger.expected_seed_consumers;
        operation_proof.gpu_index_source_direct_completed_seed_consumers =
            ledger.completed_seed_consumers;
        operation_proof.gpu_index_source_direct_required_lookups =
            ledger.cache_required_lookups;
        operation_proof.gpu_index_source_direct_required_hits =
            ledger.cache_required_hits;
        operation_proof.gpu_index_source_direct_required_misses =
            ledger.cache_required_lookups - ledger.cache_required_hits;
        operation_proof.gpu_index_source_direct_fallback_attempts = 0u;
        operation_proof.gpu_index_source_direct_mapped_prefix_passes =
            ledger.mapped_prefix_passes;
        operation_proof.gpu_index_source_direct_mapped_prefix_lookups =
            ledger.mapped_prefix_lookups;
        operation_proof.gpu_index_source_direct_mapped_prefix_comparisons =
            ledger.mapped_prefix_comparisons;
        operation_proof.gpu_index_source_direct_seed_slab_requested_bytes =
            ledger.slab_requested_bytes;
        operation_proof.
            gpu_index_source_direct_seed_slab_initial_capacity =
                ledger.slab_capacity_before;
        operation_proof.gpu_index_source_direct_seed_slab_actual_bytes =
            ledger.slab_actual_bytes;
        operation_proof.gpu_index_source_direct_seed_slab_retained_bytes =
            ledger.slab_retained_permit_bytes;
        operation_proof.
            gpu_index_source_direct_seed_slab_permit_releases =
                ledger.slab_permit_releases;
        operation_proof.gpu_index_source_direct_seed_reserved_bytes =
            ledger.seed_reserved_bytes;
        operation_proof.gpu_index_source_direct_seed_planned_vulkan_bytes =
            ledger.seed_planned_vulkan_bytes;
        operation_proof.gpu_index_source_direct_seed_actual_vulkan_bytes =
            ledger.seed_actual_vulkan_bytes;
        operation_proof.gpu_index_source_direct_seed_roundtrips =
            ledger.seed_roundtrips;
        operation_proof.gpu_index_source_direct_zero_anchor_bypasses =
            ledger.zero_anchor_bypasses;
        operation_proof.gpu_index_source_direct_seed_fence_completions =
            ledger.seed_fence_completions;
        operation_proof.gpu_index_source_direct_cache_cleanup_releases =
            ledger.cache_cleanup_releases;
        operation_proof.gpu_index_source_direct_resident_cleanup_bytes =
            ledger.resident_cleanup_bytes;
        operation_proof.gpu_index_source_direct_pipeline_completions =
            ledger.pipeline_completed ? 1u : 0u;
        if (!ledger.canonical_empty) {
            operation_proof.gpu_index_materializations = 1u;
            operation_proof.gpu_index_sorted_dense_materializations = 1u;
            operation_proof.gpu_index_unique_keys = ledger.distinct_count;
            operation_proof.gpu_index_slot_capacity = ledger.distinct_count;
            operation_proof.gpu_index_retained_minimizer_count =
                ledger.minimizer_count;
            operation_proof.gpu_index_distinct_minimizer_count =
                ledger.distinct_count;
            operation_proof.gpu_index_logical_resident_bytes =
                ledger.logical_resident_bytes;
            operation_proof.gpu_index_actual_allocation_bytes =
                ledger.materialization.actual_allocation_bytes;
            operation_proof.gpu_index_fully_touched_bytes =
                ledger.materialization.fully_touched_bytes;
            const bool reflected_authority =
                ledger.materialization.authority
                    == bioseq::indexing::GpuResidentAccountingAuthority::
                        reflected_process_footprint;
            operation_proof.gpu_index_permit_releases =
                reflected_authority ? 1u : 0u;
            operation_proof.gpu_index_process_footprint_reflections =
                reflected_authority ? 1u : 0u;
            operation_proof.gpu_index_reflected_footprint_bytes =
                ledger.materialization.reflected_footprint_bytes;
            operation_proof.gpu_index_max_load_ppm = 1'000'000u;
            operation_proof.gpu_index_max_probe = 0u;
        }
        // Seed lookup dispatch and biological-anchor proof were already
        // recorded by SeedHitStage. This operation proof deliberately leaves
        // those generic fields zero so aggregation cannot double count them.
        result.record_partial_vulkan_execution_proof(operation_proof);
        return result;
    } catch (...) {
        const std::exception_ptr pending = std::current_exception();
        bool cleanup_failed = false;
        construction_handle.reset();
        try {
            if (operation_gpu_cache) {
                const std::uint64_t resident_bytes =
                    operation_gpu_cache->resident_bytes();
                operation_gpu_cache->clear();
                if (resident_bytes != 0u) {
                    ledger.cache_cleanup_releases = 1u;
                    ledger.resident_cleanup_bytes = resident_bytes;
                }
            }
        } catch (...) {
            cleanup_failed = true;
        }
        if (!ledger.source_release_completed) {
            try {
                release_fixed_vulkan_source_after_resident_publication(
                    target_host, ledger, ledger.resident_published);
            } catch (...) {
                cleanup_failed = true;
            }
        }
        if (ledger.planning_receipts == 0u) {
            ledger.log_planning("reject", "not_reached_sort_or_count_failure");
        }
        ledger.log_terminal(
            "failure",
            cleanup_failed
                ? "operation_exception_cleanup_failed"
                : "operation_exception");
        std::rethrow_exception(pending);
    }
}

// =============================================================================
// DOC0256 §5.10.4 / §6.3 — sketch leaf and helpers
// =============================================================================
//
// The two minimizer sketching switches (target pass / query pass) in the
// legacy `minimap_port_with_repr` body collapse into:
//   - `sketch_leaf<MinimizerT, Collector, Range, MA>` — the chain's
//     terminal leaf; pins MA at compile time and invokes the trait's
//     kernel directly.
//   - `sketch_traits<MinimizerT, Collector, Range>::leaf<MA>` — the
//     alias-template wrapper exposing the leaf as a `template <auto>
//     class`.
//   - `sketch_one<MinimizerT>(...)` — one `dispatch_chain` instantiation
//     over `ma_step`.
//   - `sketch_into_collector<MinimizerT>(...)` — packages the per-pass
//     register_region_static + reserve + sketch_one into one inline call.

/// One operation-originated sketch proof, independent of whether the
/// dispatcher selected scalar, ExactSimd, or Vulkan. The Vulkan member carries
/// only a role-neutral sketch token here; the caller that knows whether this
/// invocation sketched a query or target consumes it into the corresponding
/// MatchResultCollection stage counter.
struct MinimizerSketchExecutionProof {
    ScalarMinimizerExecutionProof scalar{};
    bool scalar_present = false;
    TwoStackExecutionProof exact_simd{};
    bool exact_simd_present = false;
    PackedPaperNthashExecutionProof paper_nthash{};
    bool paper_nthash_present = false;
    PartialVulkanExecutionProof partial_vulkan{};

    [[nodiscard]] bool empty() const noexcept {
        return !scalar_present
            && scalar == ScalarMinimizerExecutionProof{}
            && !exact_simd_present
            && exact_simd == TwoStackExecutionProof{}
            && !paper_nthash_present
            && paper_nthash == PackedPaperNthashExecutionProof{}
            && partial_vulkan == PartialVulkanExecutionProof{};
    }
};

/// Transfer a fixed Vulkan host vector and whichever one accounting authority
/// it owns into a single return value.  Dense vectors normally arrive with a
/// footprint reconciliation and no permit.  Sparse non-empty vectors may carry
/// their exact permit because allocator-page reuse made the RSS delta
/// inconclusive.  Device ownership must be unique and is destroyed here before
/// any later CPU-index admission can begin.
template <class MinimizerT>
[[nodiscard]] FixedVulkanTargetHostOwner<MinimizerT>
adopt_fixed_vulkan_sketch_host_result(
    bioseq::gpu::vulkan::VulkanSketchResult<MinimizerT> source)
{
    using bioseq::gpu::vulkan::VulkanRuntimeError;
    using bioseq::gpu::vulkan::VulkanSketchHostOwnershipRoute;
    using bioseq::parallelism::saturating_mul;

    if (source.host_ownership_route
            != VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption
        || !source.arena_trimmed_before_host_materialization
        || !source.dispatch_token.accepted()) {
        throw VulkanRuntimeError{
            "Vulkan fixed host result lacks zero-copy ownership authority"};
    }
    const std::uint64_t host_capacity_bytes = saturating_mul(
        static_cast<std::uint64_t>(source.host_minimizers.capacity()),
        static_cast<std::uint64_t>(sizeof(MinimizerT)));
    const bool valid_retained_permit = source.host_permit.has_value()
        && static_cast<bool>(*source.host_permit)
        && source.host_permit->bytes() == host_capacity_bytes;
    const bool footprint_authority =
        source.host_permit_released_to_process_footprint
        && !source.host_permit.has_value()
        && (host_capacity_bytes == 0u
            || source.host_footprint_reflected_delta_bytes
                >= host_capacity_bytes);
    const bool retained_permit_authority =
        !source.host_permit_released_to_process_footprint
        && host_capacity_bytes != 0u
        && valid_retained_permit;
    const bool device_owner_present = source.device != nullptr;
    const bool device_owner_unique = device_owner_present
        && source.device.use_count() == 1u;
    if (host_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || source.host_minimizers.size()
            != source.host_minimizers.capacity()
        || footprint_authority == retained_permit_authority
        || !device_owner_unique) {
        throw VulkanRuntimeError{
            "Vulkan fixed host result has inconsistent host/device ownership "
            "authority"};
    }

    std::weak_ptr<bioseq::gpu::vulkan::
        VulkanDeviceMinimizerStorage<MinimizerT>> device_lifetime =
            source.device;
    VulkanSketchHostAdoptionReceipt<MinimizerT> receipt{
        .source_data = source.host_minimizers.data(),
        .adopted_size = source.host_minimizers.size(),
        .host_capacity_bytes = host_capacity_bytes,
        .retained_host_permit_bytes = retained_permit_authority
            ? source.host_permit->bytes()
            : 0u,
        .host_footprint_reflected_delta_bytes =
            source.host_footprint_reflected_delta_bytes,
        .device_allocation_bytes_released =
            source.device->words.allocation_bytes,
        .host_permit_released_to_process_footprint =
            source.host_permit_released_to_process_footprint,
        .device_owner_present_before_adoption = device_owner_present,
        .device_owner_unique_before_adoption = device_owner_unique,
        .arena_trimmed_before_adoption =
            source.arena_trimmed_before_host_materialization,
        .dispatch_token = source.dispatch_token,
    };
    FixedVulkanTargetHostOwner<MinimizerT> adopted;
    adopted.minimizers.swap(source.host_minimizers);
    if (source.host_permit.has_value()) {
        adopted.host_permit.emplace(std::move(*source.host_permit));
        source.host_permit.reset();
    }
    source.device.reset();
    receipt.adopted_data = adopted.minimizers.data();
    receipt.adopted_capacity = adopted.minimizers.capacity();
    receipt.source_capacity_after_adoption =
        source.host_minimizers.capacity();
    receipt.device_released_before_return = device_lifetime.expired();
    adopted.adoption = receipt;
    if (!adopted.ownership_authority_accepted()) {
        throw VulkanRuntimeError{
            "Vulkan fixed host result was not adopted zero-copy"};
    }
    return adopted;
}

template <class MinimizerT>
struct FixedVulkanTargetSketch {
    FixedVulkanTargetHostOwner<MinimizerT> host;
    MinimizerSketchExecutionProof proof{};

    FixedVulkanTargetSketch(
            FixedVulkanTargetHostOwner<MinimizerT> adopted_host,
            MinimizerSketchExecutionProof execution_proof) noexcept
        : host(std::move(adopted_host)), proof(execution_proof) {}
    FixedVulkanTargetSketch(const FixedVulkanTargetSketch&) = delete;
    FixedVulkanTargetSketch& operator=(const FixedVulkanTargetSketch&) = delete;
    FixedVulkanTargetSketch& operator=(FixedVulkanTargetSketch&&) = delete;
    FixedVulkanTargetSketch(FixedVulkanTargetSketch&& other) noexcept
        : host(std::move(other.host)), proof(other.proof) {}
};

template <class MinimizerT, class Range>
[[nodiscard]] FixedVulkanTargetSketch<MinimizerT>
sketch_vulkan_into_adopted_host_vector(
    Range& range,
    const std::size_t& window_len,
    const std::size_t& kmer_len,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_sketch");
    auto _scope = bioseq::instr::region(_desc);
    using Kernel = typename ::bioseq::simd::HWY_NAMESPACE::pipe::
        algorithm_traits<MinimizerAlgorithmType::VULKAN_MINIMAP_INT>::
            kernel_type;
    auto source = Kernel::template call_result<MinimizerT>(
        params,
        range,
        window_len,
        kmer_len,
        bioseq::gpu::vulkan::VulkanSketchHostOwnershipRoute::
            fixed_zero_copy_adoption);
    if (!source.has_value()) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError{
            "Vulkan fixed host sketch returned no result"};
    }
    auto adopted = adopt_fixed_vulkan_sketch_host_result(
        std::move(*source));
    MinimizerSketchExecutionProof proof{};
    proof.partial_vulkan.sketch_dispatches =
        adopted.adoption.dispatch_token.completed_roundtrips;
    return FixedVulkanTargetSketch<MinimizerT>{
        std::move(adopted), proof};
}

[[nodiscard]] inline bool minimizer_sketch_proof_matches_selected_route(
    ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType selected,
    const MinimizerSketchExecutionProof& proof) noexcept
{
    if (selected
        == ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR) {
        return proof.scalar_present
            && proof.scalar.accepted()
            && !proof.exact_simd_present
            && proof.exact_simd == TwoStackExecutionProof{}
            && !proof.paper_nthash_present
            && proof.paper_nthash == PackedPaperNthashExecutionProof{}
            && proof.partial_vulkan == PartialVulkanExecutionProof{};
    }
    if (selected
        == ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::TWO_STACK_NTHASH) {
        return !proof.scalar_present
            && proof.scalar == ScalarMinimizerExecutionProof{}
            && proof.exact_simd_present
            && proof.exact_simd.accepted(
                /*require_nonempty_vector_work=*/false)
            && !proof.paper_nthash_present
            && proof.paper_nthash == PackedPaperNthashExecutionProof{}
            && proof.partial_vulkan == PartialVulkanExecutionProof{};
    }
    if (selected
        == ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::PAPER_NTHASH_SIMD) {
        return !proof.scalar_present
            && proof.scalar == ScalarMinimizerExecutionProof{}
            && !proof.exact_simd_present
            && proof.exact_simd == TwoStackExecutionProof{}
            && proof.paper_nthash_present
            && proof.paper_nthash.accepted(
                /*require_nonempty_vector_work=*/false)
            && proof.partial_vulkan == PartialVulkanExecutionProof{};
    }
    if (selected
        == ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::VULKAN_MINIMAP_INT) {
        return !proof.scalar_present
            && proof.scalar == ScalarMinimizerExecutionProof{}
            && !proof.exact_simd_present
            && proof.exact_simd == TwoStackExecutionProof{}
            && !proof.paper_nthash_present
            && proof.paper_nthash == PackedPaperNthashExecutionProof{}
            && proof.partial_vulkan.exact_sketch_token_accepted();
    }
    return proof.empty();
}

enum class MinimizerSketchRole : std::uint8_t {
    query,
    target,
};

/// Consume one already-validated operation proof into its call-site role.
/// This function never consults the algorithm selector and never manufactures
/// evidence from route identity: only counters carried by the returned proof
/// are transferred.
inline void record_minimizer_sketch_execution_proof(
    MatchResultCollection& destination,
    const MinimizerSketchExecutionProof& proof,
    MinimizerSketchRole role) noexcept
{
    if (proof.scalar_present) {
        if (role == MinimizerSketchRole::query) {
            destination.record_scalar_query_minimizer_execution_proof(
                proof.scalar);
        } else {
            destination.record_scalar_target_minimizer_execution_proof(
                proof.scalar);
        }
    }
    if (proof.exact_simd_present) {
        if (role == MinimizerSketchRole::query) {
            destination.record_exact_simd_query_execution_proof(
                proof.exact_simd);
        } else {
            destination.record_exact_simd_target_execution_proof(
                proof.exact_simd);
        }
    }
    if (proof.paper_nthash_present) {
        if (role == MinimizerSketchRole::query) {
            destination.record_paper_nthash_query_execution_proof(
                proof.paper_nthash);
        } else {
            destination.record_paper_nthash_target_execution_proof(
                proof.paper_nthash);
        }
    }
    if (proof.partial_vulkan.exact_sketch_token_accepted()) {
        PartialVulkanExecutionProof routed{};
        if (role == MinimizerSketchRole::query) {
            routed.query_sketch_dispatches =
                proof.partial_vulkan.sketch_dispatches;
        } else {
            routed.target_sketch_dispatches =
                proof.partial_vulkan.sketch_dispatches;
        }
        destination.record_partial_vulkan_execution_proof(routed);
    }
}

template <class MinimizerT,
          class Collector, class Range,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType MA>
struct sketch_leaf {
    static_assert(std::is_same_v<decltype(MA),
        ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>,
        "sketch_leaf: MA must be MinimizerAlgorithmType (from ma_step).");

    BIOSEQ_PIPELINE_INLINE_ATTR
    static MinimizerSketchExecutionProof call(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        Collector& collector, Range& range,
        const std::size_t& window_len, const std::size_t& kmer_len)
    {
        using Kernel = typename ::bioseq::simd::HWY_NAMESPACE::pipe::
            algorithm_traits<MA>::kernel_type;
        if constexpr (Kernel::emits_scalar_minimizer_execution_proof) {
            return {
                .scalar = Kernel::template call<MinimizerT>(
                    params, collector, range, window_len, kmer_len),
                .scalar_present = true,
            };
        } else if constexpr (Kernel::emits_exact_simd_execution_proof) {
            return {
                .exact_simd = Kernel::template call<MinimizerT>(
                    params, collector, range, window_len, kmer_len),
                .exact_simd_present = true,
            };
        } else if constexpr (Kernel::emits_paper_nthash_execution_proof) {
            return {
                .paper_nthash = Kernel::template call<MinimizerT>(
                    params, collector, range, window_len, kmer_len),
                .paper_nthash_present = true,
            };
        } else if constexpr (Kernel::emits_partial_vulkan_execution_proof) {
            const auto token = Kernel::template call<MinimizerT>(
                params, collector, range, window_len, kmer_len);
            if (!token.accepted()) {
                ::bioseq::simd::HWY_NAMESPACE::pipe::throw_vulkan_declined(
                    "Vulkan minimizer sketch execution token");
            }
            MinimizerSketchExecutionProof proof{};
            proof.partial_vulkan.sketch_dispatches =
                token.completed_roundtrips;
            return proof;
        } else {
            Kernel::template call<MinimizerT>(
                params, collector, range, window_len, kmer_len);
            return {};
        }
    }
};

template <class MinimizerT, class Collector, class Range>
struct sketch_traits {
    template <auto MA>
    using leaf = sketch_leaf<MinimizerT, Collector, Range, MA>;
};

/// One-step chain over the MA dimension — replaces the legacy switch on
/// `params.minimizer_algorithm`.  The chain pins MA at compile time and the
/// leaf dispatches via the trait's kernel.
template <class MinimizerT, class Collector, class Range>
BIOSEQ_PIPELINE_INLINE_FLATTEN_ATTR
MinimizerSketchExecutionProof sketch_one(
    Collector& collector, Range& range,
    const std::size_t& window_len, const std::size_t& kmer_len,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    using Traits = sketch_traits<MinimizerT, Collector, Range>;
    return ::bioseq::simd::HWY_NAMESPACE::pipe::detail::dispatch_chain<
        MinimizerSketchExecutionProof,
        Traits::template leaf,
        ::bioseq::simd::HWY_NAMESPACE::pipe::detail::ma_step
    >::template run<>(params, collector, range, window_len, kmer_len);
}

/// Packages the per-pass instrumentation scope + collector reserve +
/// sketch_one into one inline call.  Replaces the ~22-line block-scope
/// boilerplate repeated for the target and query passes in the legacy
/// `minimap_port_with_repr` body.
template <class MinimizerT, class Range>
BIOSEQ_PIPELINE_INLINE_ATTR
MinimizerSketchExecutionProof sketch_into_collector(
    sketch_collector_t<MinimizerT>& collector,
    Range& range,
    const std::size_t& window_len, const std::size_t& kmer_len,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_sketch");
    auto _scope = bioseq::instr::region(_desc);
    collector.reserve(
        range.bp_size() / std::max<std::size_t>(window_len, 1) + 64);
    return sketch_one<MinimizerT>(
        collector, range, window_len, kmer_len, params);
}

/**
 * @brief Per-bucket helper for the from-sequence `minimap_port` entry.
 *
 * Owns one `Arena::Chain` scope around the target minimiser sketching + index
 * build.  Sketching is dispatched via `sketch_into_collector<MinimizerT>` —
 * the per-pass switch on `params.minimizer_algorithm` is now expressed as a
 * single-step `dispatch_chain` instantiation inside `sketch_one`.  Once the
 * target index is built, the function delegates to `minimap2_port_with_index`
 * so the per-query MinimizerStage inside `pipe::dispatch_pipeline` handles
 * query sketching.
 */
template <class MinimizerT,
          sequence_alphabet Alphabet>
[[nodiscard]] MatchResultCollection minimap_port_with_repr(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const Minimap2Params& params)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap_port");
    auto _scope = bioseq::instr::region(_desc);

    std::optional<context::context> owned_context;
    std::optional<context::installed_context_scope> context_install;
    if (context::current_or_null() == nullptr) {
        owned_context.emplace();
        context_install.emplace(*owned_context);
    }

    const std::size_t window_len = static_cast<std::size_t>(params.window_size);
    const std::size_t kmer_len   = static_cast<std::size_t>(params.kmer_size);

    // DOC0310 §16 — skip the host target sketch for the Phase C GPU-index leaf;
    // the device index build sketches the target itself. If that selected GPU
    // path cannot run, the pipeline throws instead of rebuilding on CPU.
    // The fixed Vulkan host vector carries either footprint authority or its
    // exact retained permit. Its dedicated handoff owns the later CPU-index
    // permit locally and destroys the nested index before releasing it.
    std::vector<MinimizerT> target_minimizers;
    std::optional<FixedVulkanTargetSketch<MinimizerT>> fixed_target_sketch;
    MinimizerSketchExecutionProof target_sketch_proof{};
    const bool phase_c_gpu_index =
        ::bioseq::simd::HWY_NAMESPACE::pipe::phase_c_index_build_active(params);
    using TargetAlphabet = Alphabet;
    const bool fixed_target_zero_copy_route =
        fixed_vulkan_target_host_adoption_route_selected<MinimizerT>(
            phase_c_gpu_index,
            params,
            static_cast<std::uint32_t>(
                seqan3::alphabet_size<TargetAlphabet>),
            static_cast<std::uint64_t>(target_seq.bp_size()));
    if (!phase_c_gpu_index) {
        if (fixed_target_zero_copy_route) {
            fixed_target_sketch.emplace(
                sketch_vulkan_into_adopted_host_vector<MinimizerT>(
                    target_seq, window_len, kmer_len, params));
            target_sketch_proof = fixed_target_sketch->proof;
        } else {
            sketch_collector_t<MinimizerT> target_collector;
            target_sketch_proof = sketch_into_collector<MinimizerT>(
                target_collector, target_seq, window_len, kmer_len, params);
            target_minimizers =
                std::move(target_collector).extract_sketches_data();
        }
    }

    const std::uint64_t query_sketch_started_ns =
        pipe::host_stage_now_ns();
    // Query results are intentionally left on the collector-compatible route.
    // Small/repeated allocations can reuse already-resident allocator pages,
    // so a footprint-delta release is not reliable authority for them.
    sketch_collector_t<MinimizerT> query_collector;
    const MinimizerSketchExecutionProof query_sketch_proof =
        sketch_into_collector<MinimizerT>(
            query_collector, query_seq, window_len, kmer_len, params);
    const auto query_minimizers =
        std::move(query_collector).extract_sketches_data();
    const std::uint64_t query_sketch_host_wall_ns =
        pipe::elapsed_host_stage_ns(query_sketch_started_ns);
    const bool query_route_valid =
        minimizer_sketch_proof_matches_selected_route(
            params.minimizer_algorithm, query_sketch_proof);
    const bool target_route_valid = phase_c_gpu_index
        || minimizer_sketch_proof_matches_selected_route(
            params.minimizer_algorithm, target_sketch_proof);
    const bool exact_simd_pair_route_compatible = phase_c_gpu_index
        || params.minimizer_algorithm
            != MinimizerAlgorithmType::TWO_STACK_NTHASH
        || (query_sketch_proof.exact_simd_present
            && target_sketch_proof.exact_simd_present
            && pipe::exact_simd_proofs_share_selection_route(
                query_sketch_proof.exact_simd,
                target_sketch_proof.exact_simd));
    if (!query_route_valid
        || !target_route_valid
        || !exact_simd_pair_route_compatible) {
        bioseq_log<ERR>(
            "Selected minimizer route returned mismatched execution proof or "
            "incompatible ExactSimd selection schemes");
        std::abort();
    }

    auto owned_target_minimizers = std::move(target_minimizers);
    std::vector<MinimizerT>{}.swap(target_minimizers);
    MatchResultCollection result = fixed_target_zero_copy_route
        ? minimap2_port_with_fixed_vulkan_target_handoff<
              MinimizerT, Alphabet>(
                  query_seq,
                  target_seq,
                  query_minimizers,
                  std::move(fixed_target_sketch->host),
                  params)
        : minimap2_port<MinimizerT, Alphabet>(
              query_seq,
              target_seq,
              query_minimizers,
              std::move(owned_target_minimizers),
              params);
    result.record_query_sketch_host_time(query_sketch_host_wall_ns);
    record_minimizer_sketch_execution_proof(
        result, query_sketch_proof, MinimizerSketchRole::query);
    if (!phase_c_gpu_index) {
        record_minimizer_sketch_execution_proof(
            result, target_sketch_proof, MinimizerSketchRole::target);
    }
    return result;
}

// =============================================================================
// DOC0256 §4.6 — from-sequence cascade leaf and traits
// =============================================================================
//
// `minimap_port` now collapses to one `dispatch_chain<...>::run<>(...)` over
// two steps (L, K).  The leaf below pins `(L, K)` into the
// `MinimizerRepr<L, K>` storage type and delegates to the existing
// `minimap_port_with_repr<MinimizerT>` body unchanged.

template <sequence_alphabet Alphabet,
          std::size_t L, std::size_t K>
struct minimap_port_repr_leaf {
    static_assert(std::is_same_v<decltype(L), std::size_t>,
        "minimap_port_repr_leaf: L must be std::size_t (from seq_len_log2_step).");
    static_assert(std::is_same_v<decltype(K), std::size_t>,
        "minimap_port_repr_leaf: K must be std::size_t (from kmer_size_step).");

    BIOSEQ_PIPELINE_INLINE_ATTR
    static MatchResultCollection call(
        const Minimap2Params& params,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> q,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> t)
    {
        using MinimizerT = ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<L, K>;
        return minimap_port_with_repr<MinimizerT, Alphabet>(
            q, t, params);
    }
};

template <sequence_alphabet Alphabet>
struct minimap_port_traits {
    template <auto K, auto L>
    using leaf = minimap_port_repr_leaf<Alphabet, L, K>;
};

} // namespace pipe_entry_detail

/**
 * @brief Top-level minimap2 entry — raw query/target sequences only
 * (DOC0249 §9.7 / legacy `minimap_port`; DOC0256 §4.6 chain rewrite).
 *
 * One `dispatch_chain<...>::run<>(...)` line over `seq_len_log2_step` +
 * `kmer_size_step` — replaces the hand-coded `(K, L)` ladder.  Each pinned
 * `(L, K)` selects a `MinimizerRepr<L, K>` and forwards to
 * `pipe_entry_detail::minimap_port_with_repr<MinimizerT>` which sketches
 * both target and query minimisers, builds the target index, and delegates
 * to `minimap2_port_with_index` (which in turn calls
 * `pipe::dispatch_pipeline<EP::WithPrebuiltMinimizers>`).
 */
template <sequence_alphabet Alphabet>
BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR
MatchResultCollection minimap_port(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const Minimap2Params& params = Minimap2Params{})
{
    using Traits = pipe_entry_detail::minimap_port_traits<Alphabet>;
    // DOC0271 Phase 1 §5.1 — K-then-L chain order (per-K L step).
    return ::bioseq::simd::HWY_NAMESPACE::pipe::detail::dispatch_chain<
        MatchResultCollection,
        Traits::template leaf,
        ::bioseq::simd::HWY_NAMESPACE::pipe::detail::kmer_size_step,
        ::bioseq::simd::HWY_NAMESPACE::pipe::detail::seq_len_log2_step_per_k
    >::template run<>(params, query_seq, target_seq);
}

/**
 * @brief DOC0276 — memory-bounded recompute entry. Drop-in replacement for
 * `minimap_port` on the production paths, sized for arbitrarily large targets
 * on a fixed-RAM host.
 *
 * The non-segmented `minimap_port` builds ONE minimizer index over the whole
 * target (~6 B/bp at peak → ~37 GB for 6 Gbp), which swaps a 16 GB box to death.
 * This entry instead indexes the target in budget-sized SEGMENTS (≤200 Mbp core
 * + overlap, sized by `bioseq::memory::default_budget()`), pinning the 8-byte
 * `MinimapSketchData` minimizer (segment-local positions fit its 28-bit field).
 * Each segment index is globalized (`TargetIndex::add_position_offset`) and run
 * through the UNCHANGED `minimap2_port_with_index` against the FULL target, so
 * chaining + DP alignment see correct global coordinates and stream the real
 * target bytes. A result is kept only when its `target_start` lands in the
 * segment's CORE — with overlap ≥ the longest match span, every alignment is
 * fully indexed by exactly one segment, so there are no duplicates and no
 * boundary-split partials. Per-segment index memory is freed each iteration, so
 * the live footprint is ~one segment (~1.5 GB) regardless of target size.
 *
 * Limitation (documented): `max_occ` high-frequency-seed filtering is applied
 * per-segment, so a k-mer whose genome-wide occurrences are split across
 * segments can pass the filter in each. For low-repeat targets (the sample
 * generator's near-random sequences) occurrences are ~uniform and well under
 * `max_occ`, so anchors match the non-segmented path; a global pre-count would
 * restore exact semantics for highly repetitive references (future work).
 */
template <class MinimizerT>
struct SegmentFragmentEmission {
    std::size_t fragment_no = 0;
    sequence_size_t fragment_len_bp = 0;
    std::vector<MinimizerT> minimizers{};
};

template <class MinimizerT>
[[nodiscard]] std::vector<SegmentFragmentEmission<MinimizerT>>
collect_fragment_emissions_for_segment(
        const ::bioseq::indexing::SegmentUnitRange& range,
        std::span<const MinimizerT> segment_minimizers,
        sequence_size_t total_bp,
        sequence_size_t frag_bp,
        std::size_t lpu) {
    const sequence_size_t emit_base_bp =
        static_cast<sequence_size_t>(range.cs_u) * lpu;
    const std::size_t frag_lo =
        static_cast<std::size_t>(range.core_lo_bp / frag_bp);
    const std::size_t frag_hi =
        static_cast<std::size_t>((range.core_hi_bp + frag_bp - 1) / frag_bp);

    std::vector<SegmentFragmentEmission<MinimizerT>> out;
    out.reserve(frag_hi > frag_lo ? frag_hi - frag_lo : 0);
    for (std::size_t fragment_no = frag_lo; fragment_no < frag_hi; ++fragment_no) {
        const sequence_size_t fragment_lo_bp =
            static_cast<sequence_size_t>(fragment_no) * frag_bp;
        const sequence_size_t fragment_hi_bp =
            std::min(fragment_lo_bp + frag_bp, total_bp);
        const sequence_size_t claim_lo =
            std::max(fragment_lo_bp, range.core_lo_bp);
        const sequence_size_t claim_hi =
            std::min(fragment_hi_bp, range.core_hi_bp);
        if (claim_lo >= claim_hi) {
            continue;
        }

        SegmentFragmentEmission<MinimizerT> emission{};
        emission.fragment_no = fragment_no;
        emission.fragment_len_bp = fragment_hi_bp - fragment_lo_bp;
        for (const MinimizerT& minimizer : segment_minimizers) {
            const sequence_size_t global_pos =
                emit_base_bp
                + static_cast<sequence_size_t>(minimizer.last_pos());
            if (global_pos >= claim_lo && global_pos < claim_hi) {
                emission.minimizers.emplace_back(
                    minimizer.kmer(),
                    static_cast<std::uint64_t>(global_pos - fragment_lo_bp),
                    minimizer.strand());
            }
        }
        out.push_back(std::move(emission));
    }
    return out;
}

template <class MinimizerT>
struct PendingFragmentBuild {
    sequence_size_t fragment_len_bp = 0;
    std::vector<MinimizerT> minimizers{};
};

template <class FragmentStorage, class MinimizerT>
class FragmentPersistAccumulator {
public:
    FragmentPersistAccumulator(
            FragmentStorage& storage,
            sequence_size_t total_bp,
            sequence_size_t frag_bp)
        : storage_(storage),
          total_bp_(total_bp),
          frag_bp_(frag_bp),
          nfrag_(static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp)) {}

    void ingest(std::span<const SegmentFragmentEmission<MinimizerT>> emissions) {
        for (const SegmentFragmentEmission<MinimizerT>& emission : emissions) {
            PendingFragmentBuild<MinimizerT>& pending =
                pending_[emission.fragment_no];
            pending.fragment_len_bp = emission.fragment_len_bp;
            pending.minimizers.insert(
                pending.minimizers.end(),
                emission.minimizers.begin(),
                emission.minimizers.end());
        }
    }

    void flush_ready(sequence_size_t processed_core_hi_bp) {
        while (next_flush_fragment_ < nfrag_) {
            if (fragment_hi_bp(next_flush_fragment_) > processed_core_hi_bp) {
                break;
            }
            flush_one(next_flush_fragment_++);
        }
    }

    void finish() {
        while (next_flush_fragment_ < nfrag_) {
            flush_one(next_flush_fragment_++);
        }

        const std::int64_t manifest = static_cast<std::int64_t>(nfrag_);
        storage_.resize(
            bioseq::indexing::DIRECTORY_SUB_KEY,
            sizeof(std::int64_t));
        storage_.template write_to_each<std::int64_t>(
            bioseq::indexing::DIRECTORY_SUB_KEY,
            [manifest](
                    std::size_t chunk_no,
                    std::int64_t* manifest_buf,
                    std::size_t) {
                manifest_buf[0] = manifest;
                return chunk_no;
            });
    }

private:
    [[nodiscard]] sequence_size_t fragment_lo_bp(std::size_t fragment_no) const {
        return static_cast<sequence_size_t>(fragment_no) * frag_bp_;
    }

    [[nodiscard]] sequence_size_t fragment_hi_bp(std::size_t fragment_no) const {
        return std::min(fragment_lo_bp(fragment_no) + frag_bp_, total_bp_);
    }

    void flush_one(std::size_t fragment_no) {
        PendingFragmentBuild<MinimizerT> pending{};
        if (auto it = pending_.find(fragment_no); it != pending_.end()) {
            pending = std::move(it->second);
            pending_.erase(it);
        }
        if (pending.fragment_len_bp == 0) {
            pending.fragment_len_bp =
                fragment_hi_bp(fragment_no) - fragment_lo_bp(fragment_no);
        }

        MinimizerHashmapFragmentIndex frag_idx;
        frag_idx.build(pending.minimizers, pending.fragment_len_bp);

        std::vector<char> blob(frag_idx.serialize_binary_size());
        frag_idx.serialize_to_binary(blob.data());

        const auto sub_key =
            static_cast<bioseq::indexing::index_sub_key_t>(fragment_no);
        storage_.resize(sub_key, blob.size());
        storage_.template write_to_each<char>(
            sub_key,
            [&blob](std::size_t chunk_no, char* buf, std::size_t cap_bytes) {
                const std::size_t offset =
                    chunk_no * bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
                const std::size_t n =
                    offset >= blob.size()
                        ? 0
                        : std::min(cap_bytes, blob.size() - offset);
                if (n != 0) {
                    std::memcpy(buf, blob.data() + offset, n);
                }
                return chunk_no;
            });
    }

    FragmentStorage& storage_;
    sequence_size_t total_bp_ = 0;
    sequence_size_t frag_bp_ = 0;
    std::size_t nfrag_ = 0;
    std::size_t next_flush_fragment_ = 0;
    std::map<std::size_t, PendingFragmentBuild<MinimizerT>> pending_{};
};

// =============================================================================
// DOC0307 §5.3 — query sketch for persistent-index bucket selection.
// =============================================================================
//
// Holds the query minimizers produced for persistent-index bucket filtering,
// keeping the optional GPU host-result permit and device span alive for the
// caller's lifetime. The host vector feeds the existing assembly/filter helpers
// unchanged; the Vulkan branch sketches the query on the GPU so the segmented
// path's seed lookup consumes byte-identical minimizers.
template <class MinimizerT>
struct BudgetedSketchVector {
    // Reverse destruction is the ownership contract: host capacity and the
    // device lifetime anchor disappear before either reservation can release.
    std::optional<bioseq::parallelism::MemoryPermit> permit;
    std::optional<bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>> device;
    std::vector<MinimizerT> minimizers;
    ScalarMinimizerExecutionProof scalar_execution_proof{};
    bool scalar_execution_proof_present = false;
    TwoStackExecutionProof exact_simd_execution_proof{};
    bool exact_simd_execution_proof_present = false;
    PackedPaperNthashExecutionProof paper_nthash_execution_proof{};
    bool paper_nthash_execution_proof_present = false;

    BudgetedSketchVector() = default;
    BudgetedSketchVector(const BudgetedSketchVector&) = delete;
    BudgetedSketchVector& operator=(const BudgetedSketchVector&) = delete;

    BudgetedSketchVector(BudgetedSketchVector&& other) noexcept {
        steal_from_(std::move(other));
    }

    BudgetedSketchVector& operator=(BudgetedSketchVector&& other) noexcept {
        if (this != &other) {
            release_owned_memory_before_permit();
            steal_from_(std::move(other));
        }
        return *this;
    }

    void release_owned_memory_before_permit() noexcept {
        {
            std::vector<MinimizerT> empty;
            minimizers.swap(empty);
        }
        device.reset();
        permit.reset();
    }

    [[nodiscard]] std::span<const MinimizerT> as_span() const noexcept {
        return minimizers;
    }

private:
    void steal_from_(BudgetedSketchVector&& other) noexcept {
        // Transfer allocations while all source admission anchors remain live,
        // then transfer device and host permits. Swap proves the source has no
        // residual vector capacity after the permit move.
        minimizers.swap(other.minimizers);
        device = std::move(other.device);
        other.device.reset();
        permit = std::move(other.permit);
        other.permit.reset();
        scalar_execution_proof = other.scalar_execution_proof;
        scalar_execution_proof_present =
            std::exchange(other.scalar_execution_proof_present, false);
        exact_simd_execution_proof = other.exact_simd_execution_proof;
        exact_simd_execution_proof_present =
            std::exchange(other.exact_simd_execution_proof_present, false);
        paper_nthash_execution_proof = other.paper_nthash_execution_proof;
        paper_nthash_execution_proof_present =
            std::exchange(other.paper_nthash_execution_proof_present, false);
    }
};

template <class MinimizerT, sequence_alphabet Alphabet>
[[nodiscard]] BudgetedSketchVector<MinimizerT> sketch_query_for_index_selection(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    const Minimap2Params& params,
    std::size_t w,
    std::size_t k)
{
    require_representable_minimizer_query_length<MinimizerT>(
        query_seq.bp_size());
    if (params.minimizer_algorithm == MinimizerAlgorithmType::VULKAN_MINIMAP_INT)
    {
        if (!params.vulkan_enabled) {
            pipe::throw_vulkan_declined("Vulkan query minimizer sketch");
        }
        bioseq::gpu::vulkan::VulkanSketchDecline decline{};
        if (auto gpu_sketch =
                bioseq::gpu::vulkan::collect_minimizers_vulkan_result<MinimizerT>(
                    query_seq,
                    w,
                    k,
                    &decline))
        {
            BudgetedSketchVector<MinimizerT> result;
            result.minimizers.swap(gpu_sketch->host_minimizers);
            result.device = gpu_sketch->device_span();
            result.permit = std::move(gpu_sketch->host_permit);
            return result;
        }
        pipe::throw_vulkan_sketch_declined(
            "Vulkan query minimizer sketch", decline);
    }

    sketch_collector_t<MinimizerT> query_collector;
    query_collector.reserve(query_seq.bp_size() / std::max<std::size_t>(w, 1) + 64);
    // Vulkan query sketching returned above. For the existing all-SIMD tuple,
    // use the same ExactSimd continuation backend as target preparation;
    // every other non-GPU tuple retains its explicitly scalar query backend.
    const MinimizerSketchBackend cpu_query_backend =
        pipe::target_index_sketch_backend_from_params(params);
    // The SQL/bench dna4 query representation is packed in u64 units, while
    // the exact K15/W10/B8 continuation kernel consumes u32 words on NEON.
    // Reinterpret a whole resident query as u32 without copying so the same
    // persisted-index-compatible continuation scheme executes eight
    // chronological candidates per block.  This is intentionally *not* the
    // report-76 resident vector_stream selection route: its minimizer scheme
    // differs from the persisted target index and remains forbidden here.
    // The u32 view changes only the decoder lane width; scalar-oracle
    // differential tests freeze the emitted raw minimizer sequence. Native
    // little-endian word order is part of this zero-copy split; other targets
    // retain the existing generic-u64 continuation.
    const pipe::MinimizerRangeExecutionProof execution_proof =
        pipe::minimizer_dispatch_indexed_query_whole<MinimizerT>(
            query_collector,
            query_seq,
            w,
            k,
            cpu_query_backend);
    if (!pipe::minimizer_range_proof_matches_backend(
            cpu_query_backend,
            execution_proof,
            /*require_nonempty_vector_work=*/false,
            pipe::ExactSimdRangeRoute::require_continuation)) {
        throw std::runtime_error(
            "indexed query minimizer sketch did not execute the required "
            "persisted-index-compatible route");
    }
    BudgetedSketchVector<MinimizerT> result;
    auto minimizers = std::move(query_collector).extract_sketches_data();
    result.minimizers.swap(minimizers);
    result.scalar_execution_proof = execution_proof.scalar;
    result.scalar_execution_proof_present =
        execution_proof.scalar_present;
    result.exact_simd_execution_proof = execution_proof.exact_simd;
    result.exact_simd_execution_proof_present =
        execution_proof.exact_simd_present;
    result.paper_nthash_execution_proof = execution_proof.paper_nthash;
    result.paper_nthash_execution_proof_present =
        execution_proof.paper_nthash_present;
    return result;
}

template <sequence_alphabet Alphabet,
          class FragmentIndexStorage = std::nullptr_t>
[[nodiscard]] MatchResultCollection minimap_port_segmented(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
    const Minimap2Params& params = Minimap2Params{},
    FragmentIndexStorage* fragment_index_out = nullptr)
{
    using MinimizerT = MinimapSketchData;
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap_port_segmented");
    auto _scope = bioseq::instr::region(_desc);

    const std::size_t w = static_cast<std::size_t>(params.window_size);
    const std::size_t k = static_cast<std::size_t>(params.kmer_size);
    const sequence_size_t query_len  = query_seq.bp_size();
    const sequence_size_t target_len = target_seq.bp_size();
    if (query_len == 0 || target_len == 0) {
        return MatchResultCollection{};
    }

    // DOC0345 Patch D — resolve the target-index sketch backend once; every
    // target `minimizer_dispatch_range` call below passes it explicitly so no
    // target seam can silently fall back to scalar.
    const MinimizerSketchBackend target_sketch_backend =
        pipe::target_index_sketch_backend_from_params(params);

    std::optional<bioseq::indexing::GpuMinimizerIndexCache> segmented_gpu_index_cache;
    if (params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
        bioseq::gpu::vulkan::Runtime& gpu_rt = bioseq::gpu::vulkan::Runtime::instance();
        std::uint64_t limit = bioseq::gpu::vulkan::hardware_device_available(gpu_rt)
            ? gpu_rt.features().device_local_bytes / 2u
            : (std::uint64_t{512} << 20);
        if (limit < (std::uint64_t{512} << 20)) limit = std::uint64_t{512} << 20;
        if (limit > (std::uint64_t{4} << 30)) limit = std::uint64_t{4} << 30;
        segmented_gpu_index_cache.emplace(limit);
    }
    bioseq::indexing::GpuMinimizerIndexCache* segmented_gpu_index_cache_ptr =
        segmented_gpu_index_cache ? &*segmented_gpu_index_cache : nullptr;
    auto segment_gpu_identity = [](std::size_t segment_index) noexcept {
        return bioseq::indexing::GpuIndexCacheIdentity{
            .assembly_identity = 0x9e3779b97f4a7c15ULL
                ^ (static_cast<std::uint64_t>(segment_index) + 1u),
        };
    };

    // (1) Sketch the query ONCE as MinimapSketchData (query <= 2^28 bp; its
    //     positions fit the 28-bit field). Same kmer hashing as the target, so
    //     `collect_seed_hits` matches identically. DOC0307 §5.3 — the helper
    //     sketches on the GPU when VULKAN_MINIMAP_INT is selected (byte-identical
    //     minimizers) and otherwise uses the scalar CPU sketch.
    auto query_sketch =
        sketch_query_for_index_selection<MinimizerT>(query_seq, params, w, k);
    const pipe::MinimizerBatchView<MinimizerT> query_minimizers{
        .host = std::span<const MinimizerT>{
            query_sketch.minimizers.data(), query_sketch.minimizers.size()},
        .device = query_sketch.device,
    };
    const bool exact_simd_segmented_route =
        target_sketch_backend == MinimizerSketchBackend::ExactSimd;
    const bool paper_nthash_segmented_route =
        target_sketch_backend == MinimizerSketchBackend::PaperNtHashSimd;
    const bool scalar_segmented_route =
        target_sketch_backend == MinimizerSketchBackend::Scalar;
    auto record_target_range_proof =
        [exact_simd_segmented_route, paper_nthash_segmented_route,
         scalar_segmented_route](
                MatchResultCollection& collection,
                const pipe::MinimizerRangeExecutionProof& range_proof) {
            if (exact_simd_segmented_route) {
                if (!range_proof.exact_simd_present
                    || !range_proof.exact_simd.accepted(
                        /*require_nonempty_vector_work=*/false)
                    || range_proof.paper_nthash_present
                    || range_proof.scalar_present) {
                    bioseq_log<ERR>(
                        "ExactSimd segmented target range lacks actual proof");
                    std::abort();
                }
                collection.record_exact_simd_target_execution_proof(
                    range_proof.exact_simd);
            } else if (paper_nthash_segmented_route) {
                if (!range_proof.paper_nthash_present
                    || !range_proof.paper_nthash.accepted(
                        /*require_nonempty_vector_work=*/false)
                    || range_proof.exact_simd_present
                    || range_proof.scalar_present) {
                    bioseq_log<ERR>(
                        "PaperNtHashSimd segmented target range lacks actual proof");
                    std::abort();
                }
                collection.record_paper_nthash_target_execution_proof(
                    range_proof.paper_nthash);
            } else if (scalar_segmented_route) {
                if (!range_proof.scalar_present
                    || !range_proof.scalar.accepted(
                        /*require_nonempty_work=*/false)
                    || range_proof.exact_simd_present
                    || range_proof.paper_nthash_present) {
                    bioseq_log<ERR>(
                        "Scalar segmented target range lacks actual proof");
                    std::abort();
                }
                collection.record_scalar_target_minimizer_execution_proof(
                    range_proof.scalar);
            } else if (range_proof.exact_simd_present
                       || range_proof.paper_nthash_present
                       || range_proof.scalar_present) {
                bioseq_log<ERR>(
                    "Scalar/Vulkan segmented target range emitted CPU SIMD sketch proof");
                std::abort();
            }
        };
    auto finalize_segmented_proof =
        [&query_sketch, exact_simd_segmented_route,
         paper_nthash_segmented_route, scalar_segmented_route](
                MatchResultCollection collection) {
            if (exact_simd_segmented_route) {
                if (!query_sketch.exact_simd_execution_proof_present
                    || !query_sketch.exact_simd_execution_proof.accepted(
                        /*require_nonempty_vector_work=*/false)
                    || query_sketch.paper_nthash_execution_proof_present
                    || query_sketch.scalar_execution_proof_present
                    || !collection.exact_simd_target_execution_proof_present
                    || !collection.exact_simd_target_execution_proof.accepted(
                        /*require_nonempty_vector_work=*/false)
                    || collection.paper_nthash_target_execution_proof_present) {
                    bioseq_log<ERR>(
                        "ExactSimd segmented query/target proof coverage is incomplete");
                    std::abort();
                }
                collection.record_exact_simd_query_execution_proof(
                    query_sketch.exact_simd_execution_proof);
            } else if (paper_nthash_segmented_route) {
                if (!query_sketch.paper_nthash_execution_proof_present
                    || !query_sketch.paper_nthash_execution_proof.accepted(
                        /*require_nonempty_vector_work=*/false)
                    || query_sketch.exact_simd_execution_proof_present
                    || query_sketch.scalar_execution_proof_present
                    || !collection.paper_nthash_target_execution_proof_present
                    || !collection.paper_nthash_target_execution_proof.accepted(
                        /*require_nonempty_vector_work=*/false)
                    || collection.exact_simd_target_execution_proof_present) {
                    bioseq_log<ERR>(
                        "PaperNtHashSimd segmented query/target proof coverage is incomplete");
                    std::abort();
                }
                collection.record_paper_nthash_query_execution_proof(
                    query_sketch.paper_nthash_execution_proof);
            } else if (scalar_segmented_route) {
                if (!query_sketch.scalar_execution_proof_present
                    || !query_sketch.scalar_execution_proof.accepted()
                    || query_sketch.exact_simd_execution_proof_present
                    || query_sketch.paper_nthash_execution_proof_present
                    || !collection.scalar_pipeline_execution_proof_present
                    || !collection.scalar_pipeline_execution_proof
                        .target_minimizers.accepted()
                    || collection.exact_simd_target_execution_proof_present
                    || collection.paper_nthash_target_execution_proof_present) {
                    bioseq_log<ERR>(
                        "Scalar segmented query/target proof coverage is incomplete");
                    std::abort();
                }
                collection.record_scalar_query_minimizer_execution_proof(
                    query_sketch.scalar_execution_proof);
            } else if (query_sketch.exact_simd_execution_proof_present
                       || query_sketch.paper_nthash_execution_proof_present
                       || query_sketch.scalar_execution_proof_present
                       || collection.exact_simd_target_execution_proof_present
                       || collection.paper_nthash_target_execution_proof_present
                       || (collection.scalar_pipeline_execution_proof_present
                           && collection.scalar_pipeline_execution_proof
                               .target_minimizers.calls != 0)) {
                bioseq_log<ERR>(
                    "Scalar/Vulkan segmented route emitted CPU SIMD sketch proof");
                std::abort();
            }
            return collection;
        };
    boost::unordered_flat_set<std::uint64_t> query_kmer_filter;
    query_kmer_filter.reserve(query_minimizers.size());
    for (const MinimizerT& minimizer : query_minimizers) {
        query_kmer_filter.insert(minimizer.kmer());
    }
    struct QueryKmerGate {
        std::vector<std::uint32_t> words;
        std::uint64_t mask = 0u;
        bool exact = false;

        void build(
            const boost::unordered_flat_set<std::uint64_t>& filter,
            std::size_t /*kmer_size*/) {
            const unsigned gate_bits = 26u;
            exact = false;
            if (filter.empty()) {
                words.clear();
                mask = 0u;
                return;
            }
            const std::uint64_t bit_count = std::uint64_t{1} << gate_bits;
            mask = bit_count - 1u;
            words.assign(static_cast<std::size_t>(bit_count / 32u), 0u);
            for (const std::uint64_t kmer : filter) {
                const std::uint64_t bit = kmer & mask;
                words[static_cast<std::size_t>(bit >> 5u)] |=
                    std::uint32_t{1} << (bit & 31u);
            }
        }

        [[nodiscard, gnu::always_inline]] bool maybe_contains(
            std::uint64_t kmer) const noexcept
        {
            if (words.empty()) return true;
            const std::uint64_t bit = kmer & mask;
            return (words[static_cast<std::size_t>(bit >> 5u)]
                    & (std::uint32_t{1} << (bit & 31u))) != 0u;
        }

        [[nodiscard, gnu::always_inline]] bool accepts_exactly(
            std::uint64_t /*kmer*/) const noexcept
        {
            return false;
        }

        [[nodiscard]] bioseq::gpu::vulkan::VulkanSketchQueryGate
        vulkan_gate() const noexcept
        {
            return bioseq::gpu::vulkan::VulkanSketchQueryGate{
                std::span<const std::uint32_t>{words.data(), words.size()},
                static_cast<std::uint32_t>(mask)};
        }
    };
    QueryKmerGate query_kmer_gate;
    if (target_sketch_backend == MinimizerSketchBackend::VulkanSegment) {
        query_kmer_gate.build(query_kmer_filter, k);
    }
    using FilteredMinimizerPair = std::pair<std::uint64_t, std::uint64_t>;
    auto build_query_filtered_segment_index =
        [&query_kmer_filter, &query_kmer_gate, target_len](
                std::span<const MinimizerT> minimizers,
                MinimizerHashmapFragmentIndex& index,
                std::vector<FilteredMinimizerPair>& filtered_pairs,
                bool already_query_filtered = false) -> std::size_t {
        filtered_pairs.clear();
        const std::size_t filter_size = query_kmer_filter.size();
        const std::size_t reserve_base =
            filter_size > (std::numeric_limits<std::size_t>::max)() - 64u
                ? filter_size
                : filter_size + 64u;
        const std::size_t reserve_hint = std::min(minimizers.size(), reserve_base);
        if (filtered_pairs.capacity() < reserve_hint) {
            filtered_pairs.reserve(reserve_hint);
        }
        for (const MinimizerT& minimizer : minimizers) {
            const std::uint64_t kmer = minimizer.kmer();
            if (already_query_filtered
                || query_kmer_gate.accepts_exactly(kmer)
                || (query_kmer_gate.maybe_contains(kmer)
                    && query_kmer_filter.find(kmer) != query_kmer_filter.end())) {
                filtered_pairs.emplace_back(kmer, minimizer.pos_strand_encoded());
            }
        }
        index.build_from_pairs(
            std::span<FilteredMinimizerPair>{
                filtered_pairs.data(), filtered_pairs.size()},
            target_len);
        return filtered_pairs.size();
    };
    using QueryKmerFilter = decltype(query_kmer_filter);
    struct QueryFilteredSketchCollector {
        using minimizer_type = MinimizerT;

        const QueryKmerFilter* filter = nullptr;
        const QueryKmerGate* gate = nullptr;
        std::vector<MinimizerT> sketches;

        void reserve(std::size_t expected_count) {
            const std::size_t filtered_hint = filter == nullptr
                ? expected_count
                : std::min<std::size_t>(expected_count, filter->size() + 64u);
            sketches.reserve(filtered_hint);
        }
        void clear() { sketches.clear(); }
        [[gnu::always_inline]] void collect(MinimizerT&& sketch) {
            if (filter == nullptr) {
                sketches.push_back(std::move(sketch));
                return;
            }
            const std::uint64_t kmer = sketch.kmer();
            if ((gate == nullptr || gate->maybe_contains(kmer))
                && filter->find(kmer) != filter->end()) {
                sketches.push_back(std::move(sketch));
            }
        }
        [[gnu::always_inline]] void collect(const MinimizerT& sketch) {
            if (filter == nullptr) {
                sketches.push_back(sketch);
                return;
            }
            const std::uint64_t kmer = sketch.kmer();
            if ((gate == nullptr || gate->maybe_contains(kmer))
                && filter->find(kmer) != filter->end()) {
                sketches.push_back(sketch);
            }
        }
        [[nodiscard]] const std::vector<MinimizerT>& extract_sketches_data() const & {
            return sketches;
        }
        [[nodiscard]] bool append_raw_storage(
            const typename MinimizerT::storage_t* raw,
            std::size_t count) {
            if (count > sketches.max_size() - sketches.size()) {
                return false;
            }
            for (std::size_t index = 0; index < count; ++index) {
                collect(MinimizerT::from_raw_storage(raw[index]));
            }
            return true;
        }
    };
    struct QueryFilteredPairCollector {
        using minimizer_type = MinimizerT;

        const QueryKmerFilter* filter = nullptr;
        const QueryKmerGate* gate = nullptr;
        std::uint64_t position_offset = 0u;
        std::vector<FilteredMinimizerPair> pairs;

        void reserve(std::size_t expected_count) {
            const std::size_t filtered_hint = filter == nullptr
                ? expected_count
                : std::min<std::size_t>(expected_count, filter->size() + 64u);
            pairs.reserve(filtered_hint);
        }
        void clear() { pairs.clear(); }
        [[gnu::always_inline]] void collect(MinimizerT&& sketch) {
            collect(static_cast<const MinimizerT&>(sketch));
        }
        [[gnu::always_inline]] void collect(const MinimizerT& sketch) {
            const std::uint64_t kmer = sketch.kmer();
            if (filter == nullptr
                || ((gate == nullptr || gate->maybe_contains(kmer))
                    && filter->find(kmer) != filter->end())) {
                const std::uint64_t encoded =
                    ((position_offset + sketch.last_pos()) << 1u)
                    | static_cast<std::uint64_t>(sketch.strand());
                pairs.emplace_back(kmer, encoded);
            }
        }
        [[nodiscard]] bool append_raw_storage(
            const typename MinimizerT::storage_t* raw,
            std::size_t count) {
            if (count > pairs.max_size() - pairs.size()) {
                return false;
            }
            for (std::size_t index = 0; index < count; ++index) {
                collect(MinimizerT::from_raw_storage(raw[index]));
            }
            return true;
        }
    };

    // (2) Segment the target. Overlap >= longest target match span so a match
    //     starting in a segment core is fully indexed within that segment.
    constexpr std::size_t lpu = 1u;
    const sequence_size_t overlap_bp = pipe::segment_overlap_bp(query_len);

    auto run_serial_segmented =
        [target_len, query_len, lpu, w, k, &target_seq, &query_seq,
         &query_minimizers, &query_kmer_filter, &query_kmer_gate, &params, fragment_index_out,
         target_sketch_backend, segmented_gpu_index_cache_ptr,
         &segment_gpu_identity, &build_query_filtered_segment_index,
         &record_target_range_proof,
         &finalize_segmented_proof]() -> MatchResultCollection {
        MatchResultCollection merged_collection;
        std::vector<MatchResult>& merged = merged_collection.results;
        sketch_collector_t<MinimizerT> seg_collector;
        MinimizerHashmapFragmentIndex seg_index;
        std::vector<FilteredMinimizerPair> filtered_pairs;
        const auto ranges =
            pipe::segment_unit_ranges_for_backend(
                target_len, query_len, lpu, target_sketch_backend);
        constexpr sequence_size_t frag_bp = 32768u;

        bioseq_log_lazy<DBG>([&] {
            bioseq_log_progress<DBG>("[minimap_port_segmented] serial target_len={} query_len={} segments={} "
                            "| footprint={}MB avail={}MB",
                    target_len, query_len, ranges.size(),
                    bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                    bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
        });

        if constexpr (!std::is_same_v<FragmentIndexStorage, std::nullptr_t>) {
            if (fragment_index_out != nullptr) {
                FragmentPersistAccumulator<FragmentIndexStorage, MinimizerT> acc{
                    *fragment_index_out, target_len, frag_bp};
                for (std::size_t segment_index = 0;
                     segment_index < ranges.size();
                     ++segment_index) {
                    const ::bioseq::indexing::SegmentUnitRange& range = ranges[segment_index];
                    const auto [segment_lo_bp, segment_hi_bp] =
                        pipe::validated_segment_bp_bounds(
                            range, lpu, target_len);
                    seg_collector.clear();
                    seg_collector.reserve(
                        (range.ce_u - range.cs_u) * lpu
                            / std::max<std::size_t>(w, 1) + 64);
                    const auto range_proof =
                        pipe::minimizer_dispatch_range<MinimizerT>(
                        seg_collector, target_seq,
                        segment_lo_bp, segment_hi_bp, w, k,
                        target_sketch_backend);
                    record_target_range_proof(merged_collection, range_proof);
                    const std::vector<MinimizerT>& seg_minimizers =
                        seg_collector.extract_sketches_data();

                    auto emissions = collect_fragment_emissions_for_segment(
                        range,
                        std::span<const MinimizerT>(
                            seg_minimizers.data(), seg_minimizers.size()),
                        target_len,
                        frag_bp,
                        lpu);
                    acc.ingest(std::span<const SegmentFragmentEmission<MinimizerT>>(
                        emissions.data(), emissions.size()));
                    acc.flush_ready(range.core_hi_bp);

                    build_query_filtered_segment_index(
                        std::span<const MinimizerT>{
                            seg_minimizers.data(), seg_minimizers.size()},
                        seg_index,
                        filtered_pairs);
                    seg_index.add_position_offset(segment_lo_bp);

                    MatchResultCollection seg_res = minimap2_port_with_index(
                        query_seq, target_seq, query_minimizers, seg_index, params,
                        segmented_gpu_index_cache_ptr,
                        segment_gpu_identity(segment_index));
                    merged_collection.combine_execution_proof(seg_res);
                    for (auto& result : seg_res.results) {
                        if (result.target_start >= range.core_lo_bp
                            && result.target_start < range.core_hi_bp) {
                            merged.push_back(std::move(result));
                        }
                    }
                }
                acc.finish();
                return finalize_segmented_proof(std::move(merged_collection));
            }
        }

        QueryFilteredPairCollector filtered_pair_collector{
            &query_kmer_filter, &query_kmer_gate};
        for (std::size_t segment_index = 0;
             segment_index < ranges.size();
             ++segment_index) {
            const ::bioseq::indexing::SegmentUnitRange& range = ranges[segment_index];
            const auto [segment_lo_bp, segment_hi_bp] =
                pipe::validated_segment_bp_bounds(range, lpu, target_len);
            bioseq::indexing::GpuIndexCacheIdentity gpu_identity =
                segment_gpu_identity(segment_index);
            std::size_t seg_result_count = 0;
            if (target_sketch_backend == MinimizerSketchBackend::VulkanSegment) {
                filtered_pair_collector.position_offset = segment_lo_bp;
                filtered_pair_collector.clear();
                filtered_pair_collector.reserve(
                    (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                bioseq::gpu::vulkan::collect_minimizers_vulkan_segment_or_throw<MinimizerT>(
                    filtered_pair_collector,
                    target_seq,
                    segment_lo_bp,
                    segment_hi_bp,
                    w,
                    k,
                    query_kmer_gate.vulkan_gate());
                // Safe with the approximate device gate because the collector
                // still exact-confirms each emitted kmer against query_kmer_filter.
                seg_index.build_from_pairs(
                    std::span<FilteredMinimizerPair>{
                        filtered_pair_collector.pairs.data(),
                        filtered_pair_collector.pairs.size()},
                    target_len);

                MatchResultCollection seg_res = minimap2_port_with_index(
                    query_seq, target_seq, query_minimizers, seg_index, params,
                    segmented_gpu_index_cache_ptr,
                    gpu_identity);
                merged_collection.combine_execution_proof(seg_res);
                seg_result_count = seg_res.results.size();
                for (auto& result : seg_res.results) {
                    if (result.target_start >= range.core_lo_bp
                        && result.target_start < range.core_hi_bp) {
                        merged.push_back(std::move(result));
                    }
                }
            } else {
                seg_collector.clear();
                seg_collector.reserve(
                    (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                const auto range_proof =
                    pipe::minimizer_dispatch_range<MinimizerT>(
                    seg_collector, target_seq,
                    segment_lo_bp, segment_hi_bp, w, k,
                    target_sketch_backend);
                record_target_range_proof(merged_collection, range_proof);
                const std::vector<MinimizerT>& seg_minimizers =
                    seg_collector.extract_sketches_data();
                build_query_filtered_segment_index(
                    std::span<const MinimizerT>{seg_minimizers.data(), seg_minimizers.size()},
                    seg_index,
                    filtered_pairs);
                seg_index.add_position_offset(segment_lo_bp);

                MatchResultCollection seg_res = minimap2_port_with_index(
                    query_seq, target_seq, query_minimizers, seg_index, params,
                    segmented_gpu_index_cache_ptr,
                    gpu_identity);
                merged_collection.combine_execution_proof(seg_res);
                seg_result_count = seg_res.results.size();
                for (auto& result : seg_res.results) {
                    if (result.target_start >= range.core_lo_bp
                        && result.target_start < range.core_hi_bp) {
                        merged.push_back(std::move(result));
                    }
                }
            }
            bioseq_log_lazy<DBG>([&] {
                bioseq_log_progress<DBG>("[minimap_port_segmented] segment core [{},{}) done, kept {} of {} "
                                "(merged {}) | footprint={}MB avail={}MB",
                        range.core_lo_bp, range.core_hi_bp,
                        merged.size(), seg_result_count, merged.size(),
                        bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                        bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
            });
        }
        return finalize_segmented_proof(std::move(merged_collection));
    };

    auto run_vulkan_segment_coordinate_batch = [&]() -> MatchResultCollection {
        auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
        if (!bioseq::gpu::vulkan::hardware_device_available(runtime)) {
            pipe::throw_vulkan_declined("VulkanSegment resident coordinate batch");
        }
        const auto ranges =
            pipe::segment_unit_ranges_for_backend(
                target_len, query_len, lpu, target_sketch_backend);
        std::span<const MinimizerT> sketch_span{
            query_minimizers.data(), query_minimizers.size()};
        const bioseq::gpu::vulkan::Buffer* device_words =
            (query_minimizers.device && query_minimizers.device->words() != nullptr
             && query_minimizers.device->count() == sketch_span.size())
                ? query_minimizers.device->words()
                : nullptr;

        std::vector<std::shared_ptr<bioseq::indexing::GpuResidentMinimizerIndex>>
            residents;
        std::vector<bioseq::gpu::vulkan::PhaseCSeedCoordinateJob> jobs;
        std::vector<std::size_t> job_segment_indices;
        residents.reserve(ranges.size());
        jobs.reserve(ranges.size());
        job_segment_indices.reserve(ranges.size());

        const auto frozen_query_gate = pipe::freeze_phase_c_query_gate(
            query_kmer_gate.vulkan_gate());
        const auto frozen_gate_identity =
            frozen_query_gate.canonical_identity();

        for (std::size_t segment_index = 0;
             segment_index < ranges.size();
             ++segment_index) {
            const ::bioseq::indexing::SegmentUnitRange& range = ranges[segment_index];
            const auto [segment_lo_bp, segment_hi_bp] =
                pipe::validated_segment_bp_bounds(range, lpu, target_len);
            bioseq::gpu::vulkan::VulkanSketchDecline decline{};
            auto built = bioseq::gpu::vulkan::build_gpu_segment_index_on_device<MinimizerT>(
                target_seq,
                segment_lo_bp,
                segment_hi_bp,
                w,
                k,
                frozen_query_gate,
                &decline);
            if (!built) {
                pipe::throw_vulkan_sketch_declined(
                    "VulkanSegment GPU resident index build", decline);
            }
            if (!built->valid || built->position_count == 0u || built->occupied_slots == 0u) {
                continue;
            }

            auto identity = segment_gpu_identity(segment_index);
            bioseq::indexing::GpuMinimizerIndexCacheKey key{
                .source_id = 0u,
                .caller_identity = identity.assembly_identity,
                .target_len = built->target_len,
                .table_size = built->occupied_slots,
                .position_count = built->position_count,
            };
            pipe::bind_verified_gpu_segment_gate_identity(
                key, frozen_gate_identity, built->gate_identity);
            auto adopted = bioseq::indexing::adopt_gpu_built_index(
                runtime, std::move(*built), key);
            if (!adopted) {
                throw bioseq::gpu::vulkan::VulkanRuntimeError(fmt::format(
                    "VulkanSegment GPU resident index adoption rejected ({})",
                    bioseq::indexing::gpu_built_index_adoption_error_name(
                        adopted.error())));
            }
            auto resident = std::move(*adopted);
            resident->target_position_offset = segment_lo_bp;
            residents.push_back(resident);
            jobs.push_back(bioseq::gpu::vulkan::PhaseCSeedCoordinateJob{
                .slots = &resident->slots,
                .positions = &resident->positions,
                .target_len = resident->target_len,
                .slot_mask = resident->slot_mask,
                .position_count = resident->position_count,
                .bucket_count = resident->bucket_count,
                .target_position_offset = resident->target_position_offset,
            });
            job_segment_indices.push_back(segment_index);
        }
        if (jobs.empty()) {
            return MatchResultCollection{};
        }

        auto cp = params.to_chaining_params();
        std::int32_t max_dist_x = cp.max_gap;
        std::int32_t max_dist_y = cp.max_gap;
        if (max_dist_x < cp.bandwidth) max_dist_x = cp.bandwidth;
        if (max_dist_y < cp.bandwidth) max_dist_y = cp.bandwidth;
        const auto chain_key =
            bioseq::chaining::choose_vulkan_chaining_pipeline(runtime.features());
        bioseq::gpu::vulkan::PhaseCSeedQueryInputs query_inputs{
            .host_minimizer_bytes = device_words != nullptr
                ? std::span<const std::byte>{}
                : std::as_bytes(sketch_span),
            .device_minimizer_words = device_words,
            .minimizer_count = static_cast<std::uint32_t>(sketch_span.size()),
            .minimizer_word_kind =
                static_cast<std::uint32_t>(
                    bioseq::gpu::vulkan::minimizer_wire_traits<MinimizerT>::word_kind),
            .minimizer_words32_per_record =
                bioseq::gpu::vulkan::minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
            .hash_shift =
                bioseq::gpu::vulkan::minimizer_wire_traits<MinimizerT>::hash_shift,
            .pos_bits =
                bioseq::gpu::vulkan::minimizer_wire_traits<MinimizerT>::pos_bits,
        };
        const auto coords = runtime.run_phase_c_seed_to_coordinate_batch_roundtrip(
            query_inputs,
            std::span<const bioseq::gpu::vulkan::PhaseCSeedCoordinateJob>{
                jobs.data(), jobs.size()},
            query_len,
            static_cast<std::uint32_t>(params.kmer_size),
            params.max_occ,
            bioseq::gpu::vulkan::PhaseCSeedCoordinateAlgorithm::
                vulkan_tiled_dp,
            chain_key,
            max_dist_x,
            max_dist_y,
            cp.bandwidth,
            std::min<std::int32_t>(cp.max_chain_iter, 3000),
            cp.gap_penalty,
            cp.skip_penalty,
            cp.max_chain_skip,
            cp.min_chain_score,
            cp.min_seed_count,
            16u);

        std::vector<MatchResult> merged;
        merged.reserve(coords.size());
        for (std::size_t job_index = 0; job_index < coords.size(); ++job_index) {
            const auto& coord = coords[job_index];
            if (coord.valid == 0u) continue;
            const ::bioseq::indexing::SegmentUnitRange& range =
                ranges[job_segment_indices[job_index]];
            if (coord.target_start >= range.core_lo_bp
                && coord.target_start < range.core_hi_bp) {
                merged.push_back(
                    pipe::phase_c_coordinate_to_match_result(coord, query_len, target_len));
            }
        }
        return MatchResultCollection{std::move(merged)};
    };

    if (target_sketch_backend == MinimizerSketchBackend::VulkanSegment
        && pipe::phase_c_coordinate_only_active(params)
        && fragment_index_out == nullptr) {
        return finalize_segmented_proof(
            run_vulkan_segment_coordinate_batch());
    }

    auto& pool = bioseq::parallelism::GlobalPool::instance();
    if (pipe::vulkan_accelerator_requested(params)
        || !bioseq::parallelism::task_parallel(pool.mode())
        || pool.max_concurrency() <= 1
        || target_sketch_backend == MinimizerSketchBackend::VulkanSegment
        || !lazy_is_worker_shareable(query_seq)
        || !lazy_is_worker_shareable(target_seq)) {
        return run_serial_segmented();
    }

    struct SegmentWaveAdmission {
        unsigned fanout = 1;
        unsigned geometry_divisor = 1;
        sequence_size_t segment_bp = 0;
        std::size_t estimated_segments = 0;
        std::size_t estimated_waves = 0;
        std::uint64_t peak_per_segment = 0;
        bioseq::parallelism::MemoryPermit permit;
        bool admitted = false;
    };

    constexpr std::uint64_t context_reserve = std::uint64_t{8} << 20;
    auto collected_bp_upper_bound =
        [target_len, overlap_bp](sequence_size_t core_bp) -> std::uint64_t {
        const std::uint64_t core = static_cast<std::uint64_t>(core_bp);
        const std::uint64_t overlap = static_cast<std::uint64_t>(overlap_bp);
        const std::uint64_t expanded =
            bioseq::parallelism::saturating_mul(2, overlap);
        const std::uint64_t collected =
            core > (std::numeric_limits<std::uint64_t>::max)() - expanded
                ? (std::numeric_limits<std::uint64_t>::max)()
                : core + expanded;
        return std::min<std::uint64_t>(
            collected, static_cast<std::uint64_t>(target_len));
    };

    auto peak_for_segment_core_bp =
        [&collected_bp_upper_bound](sequence_size_t core_bp) {
        return bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
            collected_bp_upper_bound(core_bp)) + context_reserve;
    };

    struct SegmentWaveCandidate {
        unsigned fanout = 1;
        unsigned geometry_divisor = 1;
        sequence_size_t segment_bp = 0;
        std::size_t estimated_segments = 0;
        std::size_t estimated_waves = 0;
        std::uint64_t peak_per_segment = 0;
        std::uint64_t reserve_bytes = 0;
        std::uint64_t estimated_work_bp = 0;
    };

    auto try_admit_segment_geometry =
        [&pool, target_len, &collected_bp_upper_bound,
         &peak_for_segment_core_bp]() -> SegmentWaveAdmission {
        auto& admission =
            bioseq::parallelism::MemoryAdmissionController::instance();
        const unsigned max_fanout = pool.max_concurrency();
        if (max_fanout <= 1) {
            return SegmentWaveAdmission{};
        }

        const std::uint64_t allocatable =
            admission.allocatable_after_reservations();
        std::vector<SegmentWaveCandidate> candidates;
        candidates.reserve(static_cast<std::size_t>(max_fanout));

        for (unsigned fanout = max_fanout; fanout > 1; --fanout) {
            const unsigned geometry_divisor = fanout;
            const sequence_size_t bp =
                pipe::segment_bp_from_budget_divided(geometry_divisor);
            const std::uint64_t collected_bp = collected_bp_upper_bound(bp);
            const std::size_t estimated_segments =
                static_cast<std::size_t>(
                    (target_len + bp - 1) / std::max<sequence_size_t>(bp, 1));
            const std::uint64_t peak = peak_for_segment_core_bp(bp);

            const std::uint64_t reserve_bytes =
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(fanout), peak);
            if (reserve_bytes > allocatable) {
                continue;
            }

            const std::size_t estimated_waves =
                (estimated_segments + fanout - 1) / fanout;
            const std::uint64_t estimated_work =
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(estimated_waves),
                    collected_bp);
            candidates.push_back(SegmentWaveCandidate{
                fanout,
                geometry_divisor,
                bp,
                estimated_segments,
                estimated_waves,
                peak,
                reserve_bytes,
                estimated_work});
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const SegmentWaveCandidate& lhs, const SegmentWaveCandidate& rhs) {
                if (lhs.estimated_work_bp != rhs.estimated_work_bp) {
                    return lhs.estimated_work_bp < rhs.estimated_work_bp;
                }
                if (lhs.estimated_segments != rhs.estimated_segments) {
                    return lhs.estimated_segments < rhs.estimated_segments;
                }
                if (lhs.fanout != rhs.fanout) {
                    return lhs.fanout > rhs.fanout;
                }
                return lhs.geometry_divisor < rhs.geometry_divisor;
            });

        for (const SegmentWaveCandidate& candidate : candidates) {
            if (auto permit = admission.try_reserve(candidate.reserve_bytes)) {
                return SegmentWaveAdmission{
                    candidate.fanout,
                    candidate.geometry_divisor,
                    candidate.segment_bp,
                    candidate.estimated_segments,
                    candidate.estimated_waves,
                    candidate.peak_per_segment,
                    std::move(*permit),
                    true};
            }
        }

        return SegmentWaveAdmission{};
    };

    auto first_wave = try_admit_segment_geometry();
    if (!first_wave.admitted || first_wave.fanout <= 1) {
        return run_serial_segmented();
    }

    bioseq_log_lazy<DBG>([&] {
        bioseq_log_progress<DBG>("[minimap_port_segmented] admitted parallel geometry fanout={} "
                        "divisor={} segment_bp={} estimated_segments={} "
                        "estimated_waves={} peak_per_segment={}MB reserve={}MB "
                        "| footprint={}MB avail={}MB",
                first_wave.fanout, first_wave.geometry_divisor,
                first_wave.segment_bp, first_wave.estimated_segments,
                first_wave.estimated_waves, first_wave.peak_per_segment >> 20,
                first_wave.permit.bytes() >> 20,
                bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
    });

    const unsigned geometry_divisor = first_wave.geometry_divisor;
    auto ranges = pipe::segment_unit_ranges(target_len, query_len, lpu, geometry_divisor);
    std::vector<MatchResultCollection> seg_out(ranges.size());
    bioseq::parallelism::TaskParallelScope scope(
        bioseq::parallelism::TaskParallelRegion::segment_recompute);
    if (!scope.acquired()) {
        return run_serial_segmented();
    }

    if constexpr (!std::is_same_v<FragmentIndexStorage, std::nullptr_t>) {
        if (fragment_index_out != nullptr) {
            constexpr sequence_size_t frag_bp = 32768u;
            FragmentPersistAccumulator<FragmentIndexStorage, MinimizerT> acc{
                *fragment_index_out, target_len, frag_bp};
            MatchResultCollection merged_collection;
            std::vector<MatchResult>& merged = merged_collection.results;

            auto process_segment_serial =
                [&ranges, &target_seq, &query_seq, &query_minimizers, &params,
                 &acc, &merged_collection, &merged, target_len, frag_bp, w, k, lpu,
                 target_sketch_backend, segmented_gpu_index_cache_ptr,
                 &segment_gpu_identity, &record_target_range_proof](
                        std::size_t segment_index) {
                sketch_collector_t<MinimizerT> collector;
                MinimizerHashmapFragmentIndex index;
                const ::bioseq::indexing::SegmentUnitRange& range = ranges[segment_index];
                const auto [segment_lo_bp, segment_hi_bp] =
                    pipe::validated_segment_bp_bounds(
                        range, lpu, target_len);
                collector.reserve(
                    (range.ce_u - range.cs_u) * lpu
                        / std::max<std::size_t>(w, 1) + 64);
                const auto range_proof =
                    pipe::minimizer_dispatch_range<MinimizerT>(
                    collector, target_seq,
                    segment_lo_bp, segment_hi_bp, w, k,
                    target_sketch_backend);
                record_target_range_proof(merged_collection, range_proof);
                const std::vector<MinimizerT>& minimizers =
                    collector.extract_sketches_data();
                auto emissions = collect_fragment_emissions_for_segment(
                    range,
                    std::span<const MinimizerT>(minimizers.data(), minimizers.size()),
                    target_len,
                    frag_bp,
                    lpu);
                acc.ingest(std::span<const SegmentFragmentEmission<MinimizerT>>(
                    emissions.data(), emissions.size()));
                acc.flush_ready(range.core_hi_bp);

                index.build(minimizers, target_len);
                index.add_position_offset(segment_lo_bp);
                MatchResultCollection seg_res = minimap2_port_with_index(
                    query_seq, target_seq, query_minimizers, index, params,
                    segmented_gpu_index_cache_ptr,
                    segment_gpu_identity(segment_index));
                merged_collection.combine_execution_proof(seg_res);
                for (auto& result : seg_res.results) {
                    if (result.target_start >= range.core_lo_bp
                        && result.target_start < range.core_hi_bp) {
                        merged.push_back(std::move(result));
                    }
                }
            };

            for (std::size_t wave_lo = 0; wave_lo < ranges.size();) {
                const std::size_t wave_count = std::min<std::size_t>(
                    first_wave.fanout, ranges.size() - wave_lo);
                bioseq::parallelism::MemoryPermit wave_permit;
                if (wave_lo == 0) {
                    wave_permit = std::move(first_wave.permit);
                } else if (auto permit =
                        bioseq::parallelism::MemoryAdmissionController::instance()
                            .try_reserve(
                                bioseq::parallelism::saturating_mul(
                                    static_cast<std::uint64_t>(wave_count),
                                    first_wave.peak_per_segment))) {
                    wave_permit = std::move(*permit);
                }
                if (!wave_permit) {
                    for (std::size_t segment_index = wave_lo;
                         segment_index < ranges.size();
                         ++segment_index) {
                        process_segment_serial(segment_index);
                    }
                    wave_lo = ranges.size();
                    break;
                }

                const std::size_t current_wave_lo = wave_lo;
                const std::size_t wave_hi = wave_lo + wave_count;
                std::vector<MatchResultCollection> wave_seg_out(wave_count);
                std::vector<std::vector<SegmentFragmentEmission<MinimizerT>>>
                    wave_frag_out(wave_count);
                pool.parallel_for_with_context_tracked(
                    wave_lo, wave_hi, 1,
                    bioseq::parallelism::range_body{
                        [&ranges, &wave_seg_out, &wave_frag_out,
                         &target_seq, &query_seq, &query_minimizers, &params,
                         current_wave_lo, target_len, frag_bp, w, k, lpu,
                         target_sketch_backend, segmented_gpu_index_cache_ptr,
                         &segment_gpu_identity, &record_target_range_proof](
                                std::size_t lo, std::size_t hi) {
                        sketch_collector_t<MinimizerT> collector;
                        MinimizerHashmapFragmentIndex index;
                        for (std::size_t segment_index = lo;
                             segment_index < hi;
                             ++segment_index) {
                            const ::bioseq::indexing::SegmentUnitRange& range =
                                ranges[segment_index];
                            const auto [segment_lo_bp, segment_hi_bp] =
                                pipe::validated_segment_bp_bounds(
                                    range, lpu, target_len);
                            const std::size_t rel = segment_index - current_wave_lo;
                            collector.clear();
                            collector.reserve(
                                (range.ce_u - range.cs_u) * lpu
                                    / std::max<std::size_t>(w, 1) + 64);
                            const auto range_proof =
                                pipe::minimizer_dispatch_range<MinimizerT>(
                                collector, target_seq,
                                segment_lo_bp, segment_hi_bp, w, k,
                                target_sketch_backend);
                            const std::vector<MinimizerT>& minimizers =
                                collector.extract_sketches_data();
                            wave_frag_out[rel] =
                                collect_fragment_emissions_for_segment(
                                    range,
                                    std::span<const MinimizerT>(
                                        minimizers.data(), minimizers.size()),
                                    target_len,
                                    frag_bp,
                                    lpu);
                            index.build(minimizers, target_len);
                            index.add_position_offset(segment_lo_bp);
                            wave_seg_out[rel] = minimap2_port_with_index(
                                query_seq,
                                target_seq,
                                query_minimizers,
                                index,
                                params,
                                segmented_gpu_index_cache_ptr,
                                segment_gpu_identity(segment_index));
                            record_target_range_proof(
                                wave_seg_out[rel], range_proof);
                        }
                    }},
                    bioseq::parallelism::current_tracked_job_observer());

                for (std::size_t rel = 0; rel < wave_count; ++rel) {
                    const std::size_t segment_index = current_wave_lo + rel;
                    const ::bioseq::indexing::SegmentUnitRange& range = ranges[segment_index];
                    acc.ingest(std::span<const SegmentFragmentEmission<MinimizerT>>(
                        wave_frag_out[rel].data(), wave_frag_out[rel].size()));
                    acc.flush_ready(range.core_hi_bp);
                    merged_collection.combine_execution_proof(
                        wave_seg_out[rel]);
                    for (auto& result : wave_seg_out[rel].results) {
                        if (result.target_start >= range.core_lo_bp
                            && result.target_start < range.core_hi_bp) {
                            merged.push_back(std::move(result));
                        }
                    }
                }
                wave_lo = wave_hi;
            }

            acc.finish();
            return finalize_segmented_proof(std::move(merged_collection));
        }
    }

    std::vector<std::size_t> segment_minimizer_counts(ranges.size());
    std::vector<std::size_t> segment_filtered_counts(ranges.size());

    auto process_segment_serial =
        [&ranges, &seg_out, &target_seq, &query_seq, &query_minimizers,
         &segment_minimizer_counts, &segment_filtered_counts,
         &build_query_filtered_segment_index, &params, target_len,
         w, k, lpu, target_sketch_backend, segmented_gpu_index_cache_ptr,
         &segment_gpu_identity,
         &record_target_range_proof](std::size_t segment_index) {
        sketch_collector_t<MinimizerT> collector;
        MinimizerHashmapFragmentIndex index;
        std::vector<FilteredMinimizerPair> filtered_pairs;
        const ::bioseq::indexing::SegmentUnitRange& range = ranges[segment_index];
        const auto [segment_lo_bp, segment_hi_bp] =
            pipe::validated_segment_bp_bounds(range, lpu, target_len);
        collector.reserve(
            (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
        const auto range_proof =
            pipe::minimizer_dispatch_range<MinimizerT>(
            collector, target_seq,
            segment_lo_bp, segment_hi_bp, w, k,
            target_sketch_backend);
        const std::vector<MinimizerT>& minimizers =
            collector.extract_sketches_data();
        segment_minimizer_counts[segment_index] = minimizers.size();
        segment_filtered_counts[segment_index] =
            build_query_filtered_segment_index(
                std::span<const MinimizerT>{minimizers.data(), minimizers.size()},
                index,
                filtered_pairs);
        index.add_position_offset(segment_lo_bp);
        seg_out[segment_index] = minimap2_port_with_index(
            query_seq, target_seq, query_minimizers, index, params,
            segmented_gpu_index_cache_ptr,
            segment_gpu_identity(segment_index));
        record_target_range_proof(seg_out[segment_index], range_proof);
        bioseq_log_lazy<DBG>([&] {
            bioseq_log_progress<DBG>("[minimap_port_segmented] adaptive serial segment core [{},{}) "
                            "produced {} results | footprint={}MB avail={}MB",
                    range.core_lo_bp, range.core_hi_bp,
                    seg_out[segment_index].results.size(),
                    bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                    bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
        });
    };

    for (std::size_t wave_lo = 0; wave_lo < ranges.size();) {
        const std::size_t requested_wave_count = std::min<std::size_t>(
            first_wave.fanout, ranges.size() - wave_lo);
        std::size_t wave_count = requested_wave_count;
        bioseq::parallelism::MemoryPermit wave_permit;
        if (wave_lo == 0) {
            wave_permit = std::move(first_wave.permit);
            wave_permit.shrink_to(
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(wave_count),
                    first_wave.peak_per_segment));
        } else {
            for (std::size_t candidate = requested_wave_count; candidate > 1; --candidate) {
                if (auto permit =
                        bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
                            bioseq::parallelism::saturating_mul(
                                static_cast<std::uint64_t>(candidate),
                                first_wave.peak_per_segment))) {
                    wave_count = candidate;
                    wave_permit = std::move(*permit);
                    break;
                }
            }
        }
        if (!wave_permit) {
            bioseq_log_lazy<DBG>([&] {
                bioseq_log_progress<DBG>("[minimap_port_segmented] wave admission failed at segment {} "
                                "of {}; processing one segment serially | footprint={}MB "
                                "avail={}MB",
                        wave_lo, ranges.size(),
                        bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                        bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
            });
            process_segment_serial(wave_lo);
            ++wave_lo;
            continue;
        }
        if (wave_count != requested_wave_count) {
            bioseq_log_lazy<DBG>([&] {
                bioseq_log_progress<DBG>("[minimap_port_segmented] wave fanout reduced from {} to {} "
                                "at segment {} of {} | footprint={}MB avail={}MB",
                        requested_wave_count, wave_count, wave_lo, ranges.size(),
                        bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                        bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
            });
        }

        const std::size_t wave_hi = wave_lo + wave_count;
        pool.parallel_for_with_context_tracked(
            wave_lo, wave_hi, 1,
            bioseq::parallelism::range_body{
                [&ranges, &seg_out, &target_seq, &query_seq, &query_minimizers,
                 &segment_minimizer_counts, &segment_filtered_counts,
                 &build_query_filtered_segment_index, &params, target_len,
                 w, k, lpu, target_sketch_backend, segmented_gpu_index_cache_ptr,
                 &segment_gpu_identity,
                 &record_target_range_proof](std::size_t lo, std::size_t hi) {
                sketch_collector_t<MinimizerT> collector;
                MinimizerHashmapFragmentIndex index;
                std::vector<FilteredMinimizerPair> filtered_pairs;
                for (std::size_t segment_index = lo; segment_index < hi; ++segment_index) {
                    const ::bioseq::indexing::SegmentUnitRange& range = ranges[segment_index];
                    const auto [segment_lo_bp, segment_hi_bp] =
                        pipe::validated_segment_bp_bounds(
                            range, lpu, target_len);
                    collector.clear();
                    collector.reserve(
                        (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                    const auto range_proof =
                        pipe::minimizer_dispatch_range<MinimizerT>(
                        collector, target_seq,
                        segment_lo_bp, segment_hi_bp, w, k,
                        target_sketch_backend);
                    const std::vector<MinimizerT>& minimizers =
                        collector.extract_sketches_data();
                    segment_minimizer_counts[segment_index] = minimizers.size();
                    segment_filtered_counts[segment_index] =
                        build_query_filtered_segment_index(
                            std::span<const MinimizerT>{
                                minimizers.data(), minimizers.size()},
                            index,
                            filtered_pairs);
                    index.add_position_offset(segment_lo_bp);
                    seg_out[segment_index] = minimap2_port_with_index(
                        query_seq, target_seq, query_minimizers, index, params,
                        segmented_gpu_index_cache_ptr,
                        segment_gpu_identity(segment_index));
                    record_target_range_proof(
                        seg_out[segment_index], range_proof);
                }
            }},
            bioseq::parallelism::current_tracked_job_observer());
        wave_lo = wave_hi;
    }

    std::size_t total_segment_minimizers = 0;
    std::size_t total_filtered_minimizers = 0;
    for (std::size_t segment_index = 0; segment_index < ranges.size(); ++segment_index) {
        total_segment_minimizers += segment_minimizer_counts[segment_index];
        total_filtered_minimizers += segment_filtered_counts[segment_index];
    }
    bioseq_log_lazy<DBG>([&] {
        bioseq_log_progress<DBG>("[minimap_port_segmented] query-filtered transient index kept {} "
                        "of {} segment minimizers across {} segments | footprint={}MB "
                        "avail={}MB",
                total_filtered_minimizers, total_segment_minimizers, ranges.size(),
                bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
    });

    MatchResultCollection merged_collection;
    std::vector<MatchResult>& merged = merged_collection.results;
    for (std::size_t segment_index = 0; segment_index < ranges.size(); ++segment_index) {
        const ::bioseq::indexing::SegmentUnitRange& range = ranges[segment_index];
        merged_collection.combine_execution_proof(seg_out[segment_index]);
        for (auto& result : seg_out[segment_index].results) {
            if (result.target_start >= range.core_lo_bp
                && result.target_start < range.core_hi_bp) {
                merged.push_back(std::move(result));
            }
        }
    }
    return finalize_segmented_proof(std::move(merged_collection));
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

} // export

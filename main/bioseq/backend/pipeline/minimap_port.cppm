/**
 * @file minimap_port.cppm
 * @brief C++20 port of the minimap2 mapping pipeline.
 *
 * This module implements the core minimap2 algorithm starting AFTER minimizer
 * collection (sketch generation). It takes pre-computed minimizers and produces
 * PAF-like alignment results.
 *
 * High-level flow:
 *   1. Build a simple hash index from target minimizers
 *   2. Collect seed hits: look up query minimizers in the target index
 *   3. Chain seeds using dynamic programming (mg_lchain_dp equivalent)
 *   4. Convert chains to alignment regions (mm_gen_regs equivalent)
 *   5. Classify primary/secondary mappings (mm_set_parent equivalent)
 *   6. Filter secondary mappings (mm_select_sub equivalent)
 *   7. Perform base-level alignment using dual-affine gap DP (KSW2 equivalent)
 *   8. Compute mapping quality (mm_set_mapq equivalent)
 *   9. Return results as MatchResult (PAF-like records)
 *
 * The implementation follows the original minimap2 C code as documented in
 * MINIMAP2_DETAILS_1.md, translating to modern C++20 with:
 *   - std::vector instead of C arrays
 *   - References instead of raw pointers
 *   - static_cast instead of C-style casts
 *   - Move semantics where beneficial
 *   - constexpr where possible
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include "common.h"

#include <cstdlib>
#include <cstdint>
#include <concepts>
#include <cmath>
#include <cstring>
#include <bit>
#include <vector>
#include <array>
#include <algorithm>
#include <numeric>
#include <limits>
#include <unordered_map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>

export module bioseq.sequences.minimap_port;

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.two_stacks;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.block_aligner;
import bioseq.sequences.chaining.utils;
import bioseq.sequences.chaining.scalar;
import bioseq.sequences.chaining.simd;
import bioseq.sequences.chaining.rmq;
import bioseq.common.context;
import bioseq.storage.scanner;
import bioseq.storage.lazy;
import bioseq.storage.size;   // DOC0263 §10 — sequence_size_t for the C1 truncation audit
import bioseq.instr;
import bioseq.indexing.minimizer_hashmap;   // DOC0279 §9 — relocated MinimizerHashmapFragmentIndex (was TargetIndex)
import bioseq.indexing.minimizer_index;     // DOC0279 §7.1 — relocated MinimizerIndex concept
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.pipeline_ops;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// DOC0263 §10.1 — the canonical "any sequence position or length" type. Every
// length / absolute coordinate carried by the port uses this (uint64) so no
// position is silently truncated for sequences longer than 2^31 (C1).
using bioseq::storage::sequence_size_t;

// Import chaining types from the shared chaining module.
// AnchorRefFields / AnchorQueryFields removed per DOC0263 §2 (Anchor is now a
// flat 24 B struct, no nested ref/query sub-objects).
using bioseq::chaining::Anchor;
using bioseq::chaining::Chain;
using bioseq::chaining::ChainingMode;
using bioseq::chaining::ChainingParams;
using bioseq::chaining::SimdChainingPolicy;
using bioseq::chaining::simd_chaining_policy_name;
using bioseq::chaining::compute_chaining_score;
using bioseq::chaining::chain_anchors_scalar;
using bioseq::chaining::chain_anchors_rmq;
using bioseq::chaining::backtrack_chains;

// DOC0279 §9 / §7.1 — the relocated minimizer index + its concept. Highway-
// namespace consumers reference them cross-namespace (POD value type + concept).
using bioseq::indexing::MinimizerHashmapFragmentIndex;
using bioseq::indexing::MinimizerLookupIndex;
using bioseq::indexing::MinimizerSeedIndex;
using bioseq::indexing::MinimizerLookupSpans;
using bioseq::indexing::MinimizerPositionSpan;
using bioseq::indexing::lookup_minimizer_spans;

// ============================================================================
// Algorithm selection enums
// ============================================================================

/// Chaining algorithm — resolved at plan time from the
/// bioseq.aligner.chaining_algorithm GUC and dispatched with constexpr-if
/// inside minimap2_port_with_index so the hot loop stays branchless.
/// Values MUST match the ordinal used in the GUC enum (columnar.cppm).
enum class ChainingAlgorithmType : uint8_t {
    SCALAR_MM2      = 0,
    SIMD_MM2        = 1,
    RMQ             = 2,
    /// G3SA-style tiled DP score generation on Vulkan (DOC0304 Phase A).
    /// Requires a usable Vulkan runtime when selected by the pipeline.
    VULKAN_TILED_DP = 3,
};

/// Base-level alignment algorithm — KSW2 dual-affine DP (minimap2 default)
/// vs Highway-vectorised block aligner (single-affine). Values MUST match
/// bioseq.aligner.base_alignment_algorithm ordinals.
enum class BaseAlignmentAlgorithmType : uint8_t {
    KSW2          = 0,
    BLOCK_ALIGNER = 1,
    /// Inter-region batched KSW2 (DOC0254 §11). Packs many small KSW2
    /// gap-fills across regions into SIMD lanes and runs an inter-sequence
    /// DP. Long fills (q_max > 256 || t_max > 256) fall through to the
    /// intra-sequence striped KSW2 kernel via the same trait surface.
    KSW2_BATCHED  = 2,
};

/// Minimizer-extraction algorithm. Selected at runtime; the
/// `minimap_port` entry point dispatches on this enum with a runtime
/// branch (cannot use constexpr-if because both paths instantiate the
/// same templates, just with the simd_hash vs scalar_hash inner type,
/// and the kmer index module's `requires` clauses already enforce the
/// correct concept).
enum class MinimizerAlgorithmType : uint8_t {
    /// Scalar `collect_minimizers` — the original reference path,
    /// used for backwards compatibility with the test reference
    /// output documented in CODE_GUIDELINES.md §9.
    SCALAR = 0,
    /// Original Highway-vectorised collector (`collect_minimizers_simd`).
    /// Branchless SIMDification of minimap2's running-min + per-step
    /// rescan loop. Kept around as a slow reference point — it has
    /// been measured to be slower than the scalar baseline on the
    /// VERY_LONG / XL benchmark buckets because the rescan fires
    /// whenever ANY lane's saved min slides off the window, which is
    /// `Lanes(d)` times more frequent than the scalar branch.
    SIMD_MINIMAP2_SLOW = 1,
    /// Production minimizer collector. Two-stack / prefix-suffix-min
    /// sliding-window with dual-stream interleaved per-step body
    /// (mirrors the SIMD-Minimizers paper) and ntHash-style mix +
    /// deferred minimap_int scramble. Implemented in
    /// `kmers/two_stacks_minimizers.cppm` as
    /// `collect_minimizers_two_stack_nthash_simd`. See
    /// RESEARCH/ENGINEERING_REPORT_V2.md for the full evolution
    /// history (was TWO_STACK_NTHASH_V3 in the iteration log).
    TWO_STACK_NTHASH = 2,
    /// Vulkan minimap-int minimizer path. DOC0351 routes the active GPU sketch
    /// through fused tiled Phase C for every supported alphabet. Requires a
    /// usable Vulkan runtime when selected by the pipeline.
    VULKAN_MINIMAP_INT = 3,
    /// Bounded-memory SIMD implementation of the u32 seq-hash ntHash scheme
    /// used by the SIMD-Minimizers work: R=7 rolling hashes, wrapping
    /// forward+reverse canonical sum, and an eight-logical-stream two-stack
    /// window. This has a distinct persisted scheme identity from the legacy
    /// minimap-int/ExactSimd paths and must never consume their indexes.
    PAPER_NTHASH_SIMD = 4,
};

/// SneakySnake execution backend.  The SQL-facing path normally selects
/// AUTO, which is resolved once before pipeline execution from the existing
/// vectorization/Vulkan choices; it is intentionally not backed by a GUC.
enum class PrefilterBackendType : uint8_t {
    AUTO    = 0,
    SCALAR  = 1,
    SIMD    = 2,
    VULKAN  = 3,
};

/// Operation-originated evidence for the SneakySnake prefilter stage.
///
/// A selector never populates this value.  The stage records one dispatch only
/// after the selected backend has returned a complete result batch and the
/// per-region survivor decision has been applied.  This makes the proof useful
/// to benchmark and SQL callers that must distinguish requested policy from
/// real maze execution.
struct SneakySnakePrefilterExecutionProof {
    std::uint64_t dispatches = 0;
    std::uint64_t scalar_dispatches = 0;
    std::uint64_t simd_dispatches = 0;
    std::uint64_t vulkan_dispatches = 0;
    std::uint64_t jobs = 0;
    std::uint64_t evaluated_jobs = 0;
    std::uint64_t length_difference_jobs = 0;
    std::uint64_t unsupported_jobs = 0;
    std::uint64_t rejected_jobs = 0;
    std::uint64_t rejected_regions = 0;
    std::uint64_t surviving_regions = 0;
    bool saturated = false;

    static constexpr void add(
            std::uint64_t& destination,
            const std::uint64_t increment,
            bool& saturated_flag) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                            - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturated_flag = true;
            return;
        }
        destination += increment;
    }

    constexpr void record_batch(
            const PrefilterBackendType backend,
            const std::uint64_t job_count,
            const std::uint64_t evaluated_count,
            const std::uint64_t length_difference_count,
            const std::uint64_t unsupported_count,
            const std::uint64_t rejected_job_count,
            const std::uint64_t rejected_region_count,
            const std::uint64_t surviving_region_count) noexcept {
        add(dispatches, 1u, saturated);
        switch (backend) {
            case PrefilterBackendType::SCALAR:
                add(scalar_dispatches, 1u, saturated);
                break;
            case PrefilterBackendType::SIMD:
                add(simd_dispatches, 1u, saturated);
                break;
            case PrefilterBackendType::VULKAN:
                add(vulkan_dispatches, 1u, saturated);
                break;
            case PrefilterBackendType::AUTO:
                // AUTO must be resolved before an operation can be attested.
                saturated = true;
                break;
        }
        add(jobs, job_count, saturated);
        add(evaluated_jobs, evaluated_count, saturated);
        add(length_difference_jobs, length_difference_count, saturated);
        add(unsupported_jobs, unsupported_count, saturated);
        add(rejected_jobs, rejected_job_count, saturated);
        add(rejected_regions, rejected_region_count, saturated);
        add(surviving_regions, surviving_region_count, saturated);
    }

    constexpr void combine(
            const SneakySnakePrefilterExecutionProof& other) noexcept {
        bool overflow = saturated || other.saturated;
        add(dispatches, other.dispatches, overflow);
        add(scalar_dispatches, other.scalar_dispatches, overflow);
        add(simd_dispatches, other.simd_dispatches, overflow);
        add(vulkan_dispatches, other.vulkan_dispatches, overflow);
        add(jobs, other.jobs, overflow);
        add(evaluated_jobs, other.evaluated_jobs, overflow);
        add(length_difference_jobs, other.length_difference_jobs, overflow);
        add(unsupported_jobs, other.unsupported_jobs, overflow);
        add(rejected_jobs, other.rejected_jobs, overflow);
        add(rejected_regions, other.rejected_regions, overflow);
        add(surviving_regions, other.surviving_regions, overflow);
        saturated = overflow;
    }

    [[nodiscard]] constexpr bool accepted(
            const PrefilterBackendType expected_backend,
            const bool require_evaluated_jobs = false,
            const bool forbid_unsupported_jobs = false) const noexcept {
        if (saturated || dispatches == 0u || jobs == 0u
            || evaluated_jobs > jobs
            || length_difference_jobs > jobs - evaluated_jobs
            || unsupported_jobs
                != jobs - evaluated_jobs - length_difference_jobs
            || rejected_jobs > jobs
            || rejected_regions > (std::numeric_limits<std::uint64_t>::max)()
                    - surviving_regions
            || rejected_regions + surviving_regions == 0u
            || scalar_dispatches > dispatches
            || simd_dispatches > dispatches - scalar_dispatches
            || vulkan_dispatches
                != dispatches - scalar_dispatches - simd_dispatches
            || (require_evaluated_jobs && evaluated_jobs == 0u)
            || (forbid_unsupported_jobs && unsupported_jobs != 0u)) {
            return false;
        }
        switch (expected_backend) {
            case PrefilterBackendType::SCALAR:
                return scalar_dispatches == dispatches
                    && simd_dispatches == 0u
                    && vulkan_dispatches == 0u;
            case PrefilterBackendType::SIMD:
                return scalar_dispatches == 0u
                    && simd_dispatches == dispatches
                    && vulkan_dispatches == 0u;
            case PrefilterBackendType::VULKAN:
                return scalar_dispatches == 0u
                    && simd_dispatches == 0u
                    && vulkan_dispatches == dispatches;
            case PrefilterBackendType::AUTO:
                return false;
        }
        return false;
    }

    constexpr bool operator==(
        const SneakySnakePrefilterExecutionProof&) const noexcept = default;
};

static_assert([] {
    SneakySnakePrefilterExecutionProof proof{};
    proof.record_batch(
        PrefilterBackendType::SIMD, 4u, 2u, 1u, 1u, 1u, 1u, 2u);
    SneakySnakePrefilterExecutionProof tail{};
    tail.record_batch(
        PrefilterBackendType::SIMD, 2u, 2u, 0u, 0u, 0u, 0u, 1u);
    proof.combine(tail);
    return proof.dispatches == 2u
        && proof.jobs == 6u
        && proof.evaluated_jobs == 4u
        && proof.accepted(PrefilterBackendType::SIMD);
}());
static_assert([] {
    SneakySnakePrefilterExecutionProof proof{};
    proof.record_batch(
        PrefilterBackendType::SCALAR,
        (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)(),
        0u, 0u, 0u, 0u, 1u);
    proof.record_batch(
        PrefilterBackendType::SCALAR, 1u, 1u, 0u, 0u, 0u, 0u, 1u);
    return proof.saturated
        && proof.jobs == (std::numeric_limits<std::uint64_t>::max)()
        && !proof.accepted(PrefilterBackendType::SCALAR);
}());

[[nodiscard]] constexpr const char* chaining_algorithm_name(ChainingAlgorithmType mode) {
    switch (mode) {
        case ChainingAlgorithmType::SCALAR_MM2:      return "Minimap2 Scalar";
        case ChainingAlgorithmType::SIMD_MM2:        return "Minimap2 SIMD";
        case ChainingAlgorithmType::RMQ:             return "Minimap2 RMQ";
        case ChainingAlgorithmType::VULKAN_TILED_DP: return "Vulkan tiled DP";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* base_alignment_algorithm_name(BaseAlignmentAlgorithmType mode) {
    switch (mode) {
        case BaseAlignmentAlgorithmType::KSW2:          return "KSW2";
        case BaseAlignmentAlgorithmType::BLOCK_ALIGNER: return "Block Aligner";
        case BaseAlignmentAlgorithmType::KSW2_BATCHED:  return "KSW2 (inter-region batched)";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* minimizer_algorithm_name(MinimizerAlgorithmType mode) {
    switch (mode) {
        case MinimizerAlgorithmType::SCALAR:             return "Scalar Minimizers";
        case MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW: return "SIMD Minimap2 (slow reference)";
        case MinimizerAlgorithmType::TWO_STACK_NTHASH:   return "Two-Stack nthash SIMD Minimizers";
        case MinimizerAlgorithmType::VULKAN_MINIMAP_INT: return "Vulkan minimap-int minimizers";
        case MinimizerAlgorithmType::PAPER_NTHASH_SIMD:  return "Paper u32 ntHash SIMD Minimizers";
    }
    return "unknown";
}

// ============================================================================
// Constants — mirrors minimap2 defaults from mm_mapopt_t
// ============================================================================

struct Minimap2Params {
    int32_t kmer_size      = 15;     ///< k-mer size
    int32_t window_size    = 10;     ///< minimizer window
    int32_t max_gap        = 5000;   ///< max reference gap in chaining
    int32_t max_gap_ref    = -1;     ///< explicit ref gap (-1 = use max_gap)
    int32_t bandwidth      = 500;    ///< bandwidth for chaining & alignment
    int32_t max_chain_skip = 25;     ///< max skip for early termination in DP
    int32_t max_chain_iter = 5000;   ///< max predecessors examined per anchor (map.c)
    int32_t min_seed_count = 3;      ///< min seeds per chain
    int32_t min_chain_score= 40;     ///< min chain score
    float   chain_gap_scale= 0.8f;   ///< gap penalty scale (options.c default)
    float   chain_skip_scale=0.0f;   ///< skip penalty scale (options.c default)
    float   mask_level     = 0.5f;   ///< primary/secondary overlap threshold
    int32_t mask_len       = std::numeric_limits<int32_t>::max(); ///< max uncovered length (INT_MAX in options.c)
    float   pri_ratio      = 0.8f;   ///< secondary score ratio threshold
    int32_t best_n         = 5;      ///< max secondary alignments to keep

    // Alignment scoring — matches mm_mapopt_init() in options.c
    int32_t match_score    = 2;      ///< match score (a)
    int32_t mismatch_pen   = 4;      ///< mismatch penalty (b)
    int32_t gap_open1      = 4;      ///< short gap open (q)
    int32_t gap_ext1       = 2;      ///< short gap extend (e)
    int32_t gap_open2      = 24;     ///< long gap open (q2)
    int32_t gap_ext2       = 1;      ///< long gap extend (e2)
    int32_t sc_ambi        = 1;      ///< penalty for ambiguous base (N)
    int32_t zdrop          = 400;    ///< z-drop threshold
    int32_t end_bonus      = -1;     ///< bonus for reaching sequence ends (-1 in options.c)

    int32_t max_occ        = 500;    ///< max seed occurrence to keep
    int32_t mid_occ        = 500;    ///< mid-level seed occurrence threshold

    /// Minimum gap length between consecutive seeds before invoking KSW2.
    /// minimap2's `mm_align1` (align.c:815) skips KSW2 alignment when the gap
    /// between consecutive seeds is below this threshold AND the seed is not
    /// the last in the chain. Default 200 matches minimap2's `opt->min_ksw_len`.
    /// Setting this to 0 forces a single KSW2 over the entire seed-to-seed
    /// region, which is the legacy bioseq behaviour. The per-seed-pair gap
    /// fill replaces a single 1Mbp×1Mbp DP with many ~200bp×~200bp DPs and
    /// is the dominant speed-up for VERY_LONG sequences (~7× total improvement
    /// at 903 kbp on the bench, see ENGINEERING_REPORT.md).
    int32_t min_ksw_len    = 200;

    /// When true, perform base-level alignment via the selected aligner and
    /// produce CIGAR. Equivalent to MM_F_CIGAR flag in minimap2. Driven by
    /// whether bioseq.cigar(...) appears in the scan target list.
    bool    do_cigar            = true;

    /// When true, run estimate_divergence_from_seeds() and populate
    /// MatchResult::divergence. Driven by whether bioseq.divergence(...) is
    /// requested. Leaving this false saves the O(n log n) binary-search walk
    /// per region, with no impact on downstream fields.
    bool    compute_divergence  = true;

    /// When true, run compute_mapping_quality() and populate
    /// MatchResult::mapq. Driven by whether bioseq.mapq(...) is requested.
    /// Skipping this omits the O(regions) log-computation loop.
    bool    compute_mapq        = true;

    /// Compute the exact unit-cost edit distance represented by the emitted
    /// alignment.  This forces traceback at the SQL boundary and is used to
    /// enforce bioseq.edit(...) predicates after SneakySnake's lower-bound
    /// gate admits a candidate.
    bool    compute_edit_distance = false;

    /// Apply the authoritative strict edit predicate to aligned regions before
    /// the final primary/secondary and MAPQ pass.  This is deliberately
    /// separate from `prefilter_enabled`: SneakySnake may conservatively admit
    /// a candidate (or be unavailable for the threshold), whereas this filter
    /// always uses the exact traced edit count.  The signed exclusive bound
    /// preserves SQL `<` semantics, including the reject-all `bound <= 0`
    /// case, without unsigned conversion surprises.
    bool         exact_edit_filter_enabled = false;
    std::int64_t exact_edit_filter_exclusive_upper_bound = 0;

    /// Enable the query-scoped SneakySnake lower-bound stage.  The threshold
    /// is the largest admissible exact edit count (so SQL `edit(...) < 10`
    /// stores 9).  No heuristic divergence/bandwidth value is used here.
    bool                 prefilter_enabled   = false;
    std::uint32_t        prefilter_max_edits = 0;
    PrefilterBackendType prefilter_backend   = PrefilterBackendType::AUTO;

    /// Block aligner tunables (ignored when base_alignment_algorithm == KSW2).
    ///
    /// Sweep results on the LONG / VERY_LONG / XL CCS bench grid (M1, NEON,
    /// nthash + SIMD-DP) — Block aligner mean_ms vs KSW2 baseline at VL:
    ///
    ///   min=32 max=256: Block 191 ms — 11% slower than KSW2 (172 ms)
    ///   min=32 max=128: Block 163 ms —  1% faster
    ///   min=32 max=64:  Block 161 ms —  3% faster
    ///   min=8  max=32:  Block 139 ms — 19% faster
    ///   min=8  max=16:  Block 132 ms — 21% faster
    ///   min=8  max=64:  Block 137 ms — 20% faster
    ///   min=16 max=64:  Block 135 ms — 21% faster
    ///   min=16 max=32:  Block 129 ms — 22% faster   <-- chosen default
    ///
    /// The previous defaults (32 / 256) over-allocated DP scratch for the
    /// short gap-fill workload that dominates a typical bench: a single
    /// region opens hundreds of gap-fill calls, most under 200 bp on each
    /// axis, and the algorithm rarely needed to grow past 32 cells. With
    /// max_size pinned at 256 each call paid for an 8-buffer scratch +
    /// trace ring sized for a doubling-grow chain that almost never fired.
    /// Pinning min=16 max=32 lets the block aligner clear the +10% goal
    /// over KSW2 + SIMD-DP + two-stack-nthash on both VERY_LONG and XL.
    /// Callers with high-error inputs (e.g. ONT_LEGACY) can override via
    /// Minimap2Params; the bench grid uses these defaults verbatim.
    size_t  block_min_size      = 16;
    size_t  block_max_size      = 32;

    /// Algorithm selection for chaining. Default: SCALAR_DP for backwards compatibility.
    ChainingMode chaining_mode  = ChainingMode::SCALAR_DP;
    int32_t bandwidth_long      = 0;  ///< Long-join bandwidth (0 = disabled)

    /// Runtime chaining algorithm selector. The pipeline reads this value
    /// once and template-dispatches so the inner loop is branchless.
    ChainingAlgorithmType        chaining_algorithm        = ChainingAlgorithmType::SCALAR_MM2;
    /// Named SIMD forward-chaining semantics.  Exact is the library default;
    /// production callers must opt into the bounded policy explicitly after
    /// resolving their final chaining-algorithm override.
    SimdChainingPolicy           simd_chaining_policy      = SimdChainingPolicy::exact_visited;
    /// Runtime base-alignment algorithm selector.
    BaseAlignmentAlgorithmType   base_alignment_algorithm  = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
    /// Runtime minimizer-extraction algorithm selector. The default is
    /// scalar to match the reference output documented in
    /// CODE_GUIDELINES.md §9; the bench harness flips it to SIMD when
    /// measuring the Highway-vectorised path.
    MinimizerAlgorithmType       minimizer_algorithm       = MinimizerAlgorithmType::SCALAR;

    /// Runtime profiler toggle. Driven by bioseq.aligner.enable_profiling
    /// GUC. When true, BatchProcessor::process_sequences instantiates the
    /// pipeline with context_args_profiling_full; when false, the
    /// no-profiling instantiation runs (zero overhead).
    bool                          enable_profiling          = false;

    /// When true and base_alignment_algorithm == KSW2_BATCHED, the
    /// dispatcher routes through the cross-query deferred-align scheduling
    /// shape (DOC0254 §11.10). All queries in the outer batch collect their
    /// fills before any DP runs, producing larger buckets and better SIMD
    /// occupancy on shorter queries. Off by default — per-query bucketing
    /// already saturates the bucket fill for VERY_LONG inputs.
    bool                          cross_query_batched       = false;

    /// Enable Vulkan execution for Vulkan-backed algorithms. If a Vulkan
    /// algorithm is selected while this is false, the pipeline raises an error;
    /// algorithm fallback must be chosen before entering the pipeline.
    bool                          vulkan_enabled            = true;

    /// DOC0307 Phase B — enable the Vulkan seed-hit lookup when Vulkan is on.
    /// Runtime routing only (never a template parameter). When true, selected
    /// seed-hit GPU execution is mandatory; when false, the CPU seed path is
    /// selected before entering the GPU backend.
    bool                          vulkan_seed_hits_enabled  = false;
    /// DOC0307 Phase B — cache GPU-resident minimizer index uploads while their
    /// memory permit is held (batch/wave scoped). Runtime routing only.
    bool                          vulkan_index_cache_enabled = true;

    /// DOC0310 Phase C — resident two-submit Vulkan frontend (parallel ordered
    /// sketch + GPU seed count/emit + device anchor sort). Runtime routing only;
    /// unsupported GPU/runtime shapes are hard errors once this path is selected.
    bool                          vulkan_fused_frontend_enabled = false;
    /// DOC0310 Phase C — build the target minimizer index entirely on the GPU
    /// (parallel sketch → device radix sort → race-free slot table) instead of
    /// the CPU `MinimizerHashmapFragmentIndex` build + slot upload.
    bool                          vulkan_gpu_index_build_enabled = false;
    /// DOC0310 Phase C — optional GPU base-alignment extension under the block
    /// aligner. Disabled by default; never gates the Phase C performance path.
    bool                          vulkan_extend_enabled = false;
    /// DOC0310 Phase C — ordered-sketch tile size (base pairs per tile).
    uint32_t                      vulkan_phase_c_tile_bp = 2048;
    /// DOC0310 Phase C — device radix sort digit width (bits per pass).
    uint32_t                      vulkan_phase_c_radix_bits = 8;

    /// Build ChainingParams from this Minimap2Params.
    ///
    /// Extracts only the chaining-relevant subset and precomputes derived values.
    /// The ChainingMode (used by the shared chaining modules) is synthesized
    /// from chaining_algorithm so both selectors stay aligned.
    [[nodiscard]] ChainingParams to_chaining_params() const {
        ChainingParams cp;
        cp.kmer_size        = kmer_size;
        cp.max_gap          = max_gap;
        cp.bandwidth        = bandwidth;
        cp.max_chain_skip   = max_chain_skip;
        cp.max_chain_iter   = max_chain_iter;
        cp.min_seed_count   = min_seed_count;
        cp.min_chain_score  = min_chain_score;
        cp.chain_gap_scale  = chain_gap_scale;
        cp.chain_skip_scale = chain_skip_scale;
        cp.simd_policy      = simd_chaining_policy;
        switch (chaining_algorithm) {
            case ChainingAlgorithmType::SIMD_MM2:   cp.chaining_mode = ChainingMode::SIMD_DP; break;
            case ChainingAlgorithmType::RMQ:        cp.chaining_mode = ChainingMode::RMQ;     break;
            // VULKAN_TILED_DP maps to SCALAR_DP only because ChainingMode has no
            // Vulkan value and is not the dispatch selector; ChainStage selects
            // the kernel from chaining_algorithm.
            case ChainingAlgorithmType::VULKAN_TILED_DP:
                                                    cp.chaining_mode = ChainingMode::SCALAR_DP; break;
            case ChainingAlgorithmType::SCALAR_MM2:
            default:                                cp.chaining_mode = ChainingMode::SCALAR_DP; break;
        }
        cp.vulkan_enabled = vulkan_enabled;
        cp.coordinate_only = !do_cigar && !compute_divergence && !compute_mapq;
        cp.prepare();
        return cp;
    }
};

// CIGAR opcodes and CigarOp are imported from bioseq_cigar module.

/**
 * @brief PAF-like alignment result.
 *
 * Maps to the 12 mandatory PAF columns plus alignment details.
 */
struct MatchResult {
    // PAF mandatory columns. DOC0263 §10.2: every length / absolute coordinate
    // (cols 2,3,4,7,8,9,10,11) is `sequence_size_t` (uint64) so it is never
    // truncated for sequences longer than 2^31. PAF emits them as decimal
    // strings, so the wire width is unchanged.
    sequence_size_t query_len      = 0;    ///< col 2: query sequence length
    sequence_size_t query_start    = 0;    ///< col 3: query start (0-based, closed)
    sequence_size_t query_end      = 0;    ///< col 4: query end (0-based, open)
    bool    is_reverse             = false;///< col 5: '+' or '-'
    sequence_size_t target_len     = 0;    ///< col 7: target sequence length
    sequence_size_t target_start   = 0;    ///< col 8: target start on original strand
    sequence_size_t target_end     = 0;    ///< col 9: target end on original strand
    sequence_size_t num_matches    = 0;    ///< col 10: number of residue matches (mlen)
    sequence_size_t block_length   = 0;    ///< col 11: alignment block length (blen)
    int32_t mapq                   = 0;    ///< col 12: mapping quality (0-255)

    // Additional alignment info
    int32_t chain_score            = 0;    ///< chaining score
    int32_t dp_score               = 0;    ///< DP alignment score
    int32_t dp_max_score           = 0;    ///< max scoring segment score (dp_max0 in original)
    int32_t seed_count             = 0;    ///< number of seeds in chain
    int32_t secondary_chain_score  = 0;    ///< best secondary chaining score against this primary
    float   divergence             = -1.f; ///< sequence divergence estimate
    bool    is_primary             = true; ///< primary mapping flag
    bool    has_cigar              = false;///< whether CIGAR alignment was performed
    std::vector<CigarOp> cigar;            ///< CIGAR string
    sequence_size_t edit_distance  = 0;    ///< exact unit-cost edits in the reported alignment
};

// ============================================================================
// Internal data structures
// ============================================================================
// Anchor, AnchorRefFields, AnchorQueryFields, Chain are now imported from
// bioseq_sequences.chaining.utils via 'using' declarations above.

/**
 * @brief Alignment region — equivalent to mm_reg1_t.
 *
 * Produced after mm_gen_regs, enriched through the pipeline.
 */
struct AlignRegion {
    int32_t region_id       = 0;
    int32_t parent          = -1;   ///< parent id (-1 = unset, ==region_id => primary)
    int32_t score           = 0;    ///< chain score
    int32_t original_score  = 0;    ///< initial chain score (before merging)
    int32_t seed_count      = 0;    ///< number of seeds
    int32_t anchor_start    = 0;    ///< offset in anchor array

    // DOC0263 §10.2 — absolute coordinates / lengths are `sequence_size_t`
    // (uint64) so they carry the untruncated 64-bit anchor positions through
    // to the MatchResult. Intermediate coordinate math that can go negative
    // (e.g. ref_position + 1 - span) is done in signed int64 then clamped.
    sequence_size_t query_start = 0;    ///< query start position
    sequence_size_t query_end   = 0;    ///< query end position
    sequence_size_t ref_start   = 0;    ///< reference start position
    sequence_size_t ref_end     = 0;    ///< reference end position
    int32_t reference_id    = 0;    ///< reference sequence id (0 in our case)
    bool    is_reverse      = false;///< reverse strand

    sequence_size_t match_length = 0;   ///< seeded exact match length
    sequence_size_t block_length = 0;   ///< seeded alignment block length
    int32_t secondary_score = 0;    ///< best secondary score against this primary
    int32_t secondary_count = 0;    ///< number of suboptimal mappings

    // DP alignment results (filled after alignment phase)
    int32_t dp_score            = 0;
    int32_t dp_max_score        = 0;    ///< max scoring segment
    int32_t secondary_dp_max    = 0;    ///< best secondary DP score
    bool    has_alignment       = false;///< whether DP alignment was performed
    float   divergence          = -1.f; ///< divergence estimate

    int32_t mapq            = 0;

    std::vector<CigarOp> cigar;
    sequence_size_t edit_distance = 0;
};

// fast_log2 is now in bioseq_sequences.chaining.utils

// ============================================================================
// Phase 1: Build target index from target minimizers
// ============================================================================

// DOC0279 §9 — `TargetIndex` and the local `MinimizerIndex` concept were
// RELOCATED to module `bioseq_indexing.minimizer_hashmap`
// (`bioseq::indexing::MinimizerHashmapFragmentIndex`) and
// `bioseq_indexing.minimizer_index`. Brought into this namespace via the
// `using` declarations near the top of the Highway namespace block.


// ============================================================================
// Phase 2: Collect seed hits — look up query minimizers in target index
// ============================================================================

/**
 * Append one borrowed packed target-position run for a single query
 * minimizer. Position width is resolved once by the caller, so a Format-6
 * uint32 posting remains packed through the seed loop and is widened only in
 * the register used to construct its anchor.
 */
template <std::unsigned_integral StoredPosition>
inline void append_seed_hits_from_position_span(
    std::span<const StoredPosition> target_hits,
    std::uint32_t query_strand_bit,
    sequence_size_t forward_query_pos,
    sequence_size_t reverse_query_pos,
    std::uint32_t query_span_value,
    ::bioseq::chaining::ArenaSlab<Anchor>& anchors_out)
{
    for (const StoredPosition stored_target_hit : target_hits) {
        const std::uint64_t target_hit =
            static_cast<std::uint64_t>(stored_target_hit);
        const sequence_size_t ref_position =
            static_cast<sequence_size_t>(target_hit >> 1u);
        const std::uint32_t target_strand_bit =
            static_cast<std::uint32_t>(target_hit & 1u);
        const std::uint32_t is_reverse_bit =
            target_strand_bit ^ query_strand_bit;
        const sequence_size_t selected_query_pos =
            is_reverse_bit ? reverse_query_pos : forward_query_pos;

        Anchor anchor;
        anchor.ref_position = ref_position;
        anchor.is_reverse = is_reverse_bit;
        anchor.query_position = selected_query_pos;
        anchor.query_span = static_cast<std::uint8_t>(query_span_value);
        anchor.flags = 0;
        anchor.segment_id = 0;
        anchors_out.push_back(anchor);
    }
}

inline void sort_seed_hits(
    ::bioseq::chaining::ArenaSlab<Anchor>& anchors_out)
{
    static const auto* _sort_desc =
        bioseq::instr::register_region_static("collect_seed_hits_sort");
    auto _sort_scope = bioseq::instr::region(_sort_desc);
    if (anchors_out.size() < 2u) return;
    (void)::bioseq::chaining::sort_canonical_anchors(
        std::span<Anchor>{anchors_out.data(), anchors_out.size()});
}

/**
 * @brief Collect seed anchors by looking up query minimizers in the target index.
 *
 * Equivalent to collect_seed_hits() in map.c:518-554.
 *
 * For each query minimizer, look up its hash in the target index. For each
 * hit, create an anchor encoding both reference and query positions.
 *
 * Anchor encoding (same as minimap2):
 *   ref_bits: is_reverse<<63 | reference_id<<32 | ref_position
 *   query_bits: query_span<<32 | query_position
 *
 * Where for forward strand: ref_position = target_pos, query_position = query_pos
 * For reverse strand: set is_reverse bit, adjust query_position for reverse complement.
 *
 * @param query_minimizers Pre-computed query minimizers
 * @param target_index     Target index to search
 * @param query_len        Length of query sequence
 * @param max_occ          Max occurrences to keep (filter high-frequency seeds)
 * @param kmer_size        K-mer length used for the encoded anchor span.
 * @param anchors_out      Output slab — sorted anchors are appended here.
 *                          Must be a live ArenaSlab rooted in the caller's
 *                          per-query `Arena::Chain` scope; the kernel writes
 *                          directly into it via `push_back` and sorts in
 *                          place before returning (DOC0249 §13 follow-up:
 *                          output-side kernel-to-arena refactor).
 */
template <class MinimizerT, MinimizerLookupIndex IndexT>
void collect_seed_hits(
    std::span<const MinimizerT> query_minimizers,
    const IndexT& target_index,
    sequence_size_t query_len,
    int32_t max_occ,
    int32_t kmer_size,
    ::bioseq::chaining::ArenaSlab<Anchor>& anchors_out)
{
    static const auto* _seed_hits_desc =
        bioseq::instr::register_region_static("collect_seed_hits");
    auto _seed_hits_scope = bioseq::instr::region(_seed_hits_desc);
    // The caller is expected to pre-reserve the slab using the
    // `estimated_anchor_count` helper from `bioseq::simd::HWY_NAMESPACE::pipe::detail`
    // — that estimate is tighter than `query_minimizers.size() * 2` for very-short
    // sequences (clamped by `shared_ceiling`) and identical for the common case.
    // We still issue a defensive `reserve` here so the kernel works when invoked
    // directly with a fresh slab; the call is a no-op when `cap_` already covers
    // the requested capacity (see `ArenaSlab::reserve` short-circuit).
    anchors_out.reserve(query_minimizers.size() * 2);

    // Target-index lookup is the hot per-minimizer operation. We wrap the
    // whole loop (not each lookup) to avoid profiler overhead dominating
    // the measurement; the loop body also classifies/sorts anchors.
    {
    static const auto* _idx_lookup_desc =
        bioseq::instr::register_region_static("target_index_lookup");
    auto _idx_lookup_scope = bioseq::instr::region(_idx_lookup_desc);
    // The compact MinimizerT no longer carries a per-minimizer span field
    // — for the dna4 bench path span is identically equal to the
    // configured kmer size, so we hoist it from `kmer_size` once outside
    // the loop instead of reading a now-removed bitfield.
    const uint32_t query_span_value = static_cast<uint32_t>(kmer_size);
    // Precompute the constant added to the negated forward_query_pos in the
    // reverse-strand branch:
    //   reverse_query_pos = query_len - (fwd + 1 - span) - 1
    //                     = (query_len - 2 + span) - fwd
    // Hoisting this addition out of the inner loop lets the compiler keep
    // (sub fwd) as the only per-hit op for the reverse branch.
    const sequence_size_t reverse_pos_base = query_len - 2 + query_span_value;
    if (max_occ < 0) [[unlikely]] {
        throw std::invalid_argument(
            "collect_seed_hits requires a nonnegative max_occ");
    }
    for (const auto& query_minimizer : query_minimizers) {
        const MinimizerLookupSpans target_hits = lookup_minimizer_spans(
            target_index, query_minimizer.kmer());
        const std::uint64_t global_occurrence_count =
            target_hits.global_occurrence_count();
        if (global_occurrence_count == 0
            || global_occurrence_count
                > static_cast<std::uint64_t>(max_occ)) {
            continue;
        }
        if (!target_hits.positions_complete() || !target_hits.well_formed()) {
            throw bioseq::indexing::PersistedMinimizerLookupError(
                "persisted minimizer positions unavailable or inconsistent "
                "after global occurrence pruning");
        }

        // `pos_strand_encoded()` returns `(last_pos << 1) | strand` — the
        // exact form `lookup()` packed into the positions array, so the
        // per-hit `target_hit` lives in the same encoded space.
        // DOC0274 §1.9 — keep the FULL 64-bit encoded value. `pos_bits` is
        // `ceil(log2(seq_len))` (up to 32 for a multi-Gbp sequence), so
        // `(last_pos << 1) | strand` spans up to 33 bits; the old
        // `static_cast<uint32_t>(...)` truncated any position >= 2^31. The
        // query is small here, but mirroring the target-side fix below keeps
        // both sides 64-bit-correct.
        const uint64_t query_pos_encoded = query_minimizer.pos_strand_encoded();
        // Hoist the strand bit out of the inner per-hit loop — constant
        // across `target_hits`.
        const uint32_t query_strand_bit = static_cast<uint32_t>(query_pos_encoded & 1ULL);
        const sequence_size_t forward_query_pos = static_cast<sequence_size_t>(query_pos_encoded >> 1);
        // Reverse-strand position formula reduces to a single subtraction
        // once `reverse_pos_base` is hoisted; pre-compute here so the
        // branchless `csel` below picks between two register-resident
        // 64-bit positions. (forward_query_pos is a k-mer start <= query_len-k,
        // so reverse_pos_base - forward_query_pos never underflows.)
        const sequence_size_t reverse_query_pos = reverse_pos_base - forward_query_pos;

        const auto contiguous32 = target_hits.contiguous_positions32();
        const auto contiguous64 = target_hits.contiguous_positions();
        if (!contiguous32.empty()) {
            append_seed_hits_from_position_span(
                contiguous32,
                query_strand_bit,
                forward_query_pos,
                reverse_query_pos,
                query_span_value,
                anchors_out);
        } else if (!contiguous64.empty()) {
            append_seed_hits_from_position_span(
                contiguous64,
                query_strand_bit,
                forward_query_pos,
                reverse_query_pos,
                query_span_value,
                anchors_out);
        } else {
            for (const MinimizerPositionSpan& target_span
                 : target_hits.segmented_positions()) {
                append_seed_hits_from_position_span(
                    target_span.positions,
                    query_strand_bit,
                    forward_query_pos,
                    reverse_query_pos,
                    query_span_value,
                    anchors_out);
            }
        }
    }
    }  // end target_index_lookup scope

    // Establish the canonical `(ref_bits, query_bits, full query_position)`
    // order. The primary reference-key pass remains the exact port of
    // minimap2's `radix_sort_128x(a, a + n_a)` from misc.c; equal-reference
    // runs are then comparison-sorted by `sort_seed_hits`. The
    // 8-bit-per-pass MSD radix sort is faster than libc++ pdqsort by
    // ~3-5x at the anchor populations we see (50 k - 1 M+ at LONG/XL/XXL)
    // because it bypasses the comparison-based branch-predictor
    // pessimisation pdqsort hits when the ref_position prefix is
    // already monotonic — most of the work is just the 256-bucket
    // counting scan, which the M1 LSU pipelines well.
    //
    // A SIMD-vectorised bit-by-bit MSD radix variant (Bramas / Möller /
    // Keller, 2024 thesis) was prototyped on top of Highway's
    // `CompressBlendedStore`. On NEON-128 / Apple M1 the SIMD partition
    // is restricted to 2 u64 lanes per vector (vs. 8 on AVX-512) and
    // each NEON BlendedStore lowers to a 3-step Load+Blend+Store —
    // measured as 5–22 percentage points slower than this 8-bit MSD
    // radix sort on the bench's LONG/VLONG/XL/XXL buckets. See
    // main/DOCS/REPORTS/NEW_SPEEDUP_REPORT_2__DOC0149.md for the full V1-vs-V2
    // numbers; the SIMD variant becomes attractive again on AVX-512
    // hosts and is left as a future-work option.
    sort_seed_hits(anchors_out);

    bioseq_log_lazy<DBG>([&] {
        bioseq_log_progress<DBG>("[minimap_port] Collected {} seed anchors from {} query minimizers"
                " | footprint={}MB avail={}MB",
                anchors_out.size(), query_minimizers.size(),
                bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
    });
}

// ============================================================================
// Phase 3: Chaining — now delegated to chaining modules
// ============================================================================
// compute_chaining_score is imported from bioseq_sequences.chaining.utils
// chain_anchors_scalar is imported from bioseq_sequences.chaining.scalar
// chain_anchors_simd is imported from bioseq_sequences.chaining.simd (in bioseq::simd::HWY_NAMESPACE)
// chain_anchors_rmq is imported from bioseq_sequences.chaining.rmq

// ============================================================================
// Phase 4: Convert chains to alignment regions (mm_gen_regs equivalent)
// ============================================================================

/**
 * @brief Compute seeded match length and block length for a region.
 *
 * Equivalent to mm_cal_fuzzy_len() in hit.c.
 *
 * match_length: approximate number of matching bases in seeded regions
 * block_length: approximate total aligned length in seeded regions
 */
void compute_fuzzy_lengths(AlignRegion& region, std::span<const Anchor> anchors) {
    region.match_length = 0;
    region.block_length = 0;
    if (region.seed_count <= 0) {
        return;
    }

    const int32_t anchor_begin = region.anchor_start;
    // Accumulate in signed int64 (gaps are bounded position *deltas*, never
    // absolute positions) then store into the sequence_size_t fields.
    std::int64_t match_length = static_cast<int32_t>(anchors[anchor_begin].query_span);
    std::int64_t block_length = match_length;

    for (int32_t anchor_idx = anchor_begin + 1;
         anchor_idx < anchor_begin + region.seed_count;
         ++anchor_idx)
    {
        const int32_t span = static_cast<int32_t>(anchors[anchor_idx].query_span);
        const int32_t target_gap = static_cast<int32_t>(
            anchors[anchor_idx].ref_position - anchors[anchor_idx - 1].ref_position);
        const int32_t query_gap = static_cast<int32_t>(
            anchors[anchor_idx].query_position - anchors[anchor_idx - 1].query_position);
        const int32_t max_gap = (target_gap > query_gap) ? target_gap : query_gap;
        block_length += max_gap;
        const bool both_gaps_exceed_span = (target_gap > span && query_gap > span);
        const int32_t min_gap = (target_gap < query_gap) ? target_gap : query_gap;
        const int32_t match_increment = both_gaps_exceed_span ? span : min_gap;
        match_length += match_increment;
    }
    region.match_length = static_cast<sequence_size_t>(match_length);
    region.block_length = static_cast<sequence_size_t>(block_length);
}

/**
 * @brief Set alignment coordinates from seed positions.
 *
 * Equivalent to mm_reg_set_coor() in hit.c.
 * Derives query_start, query_end, ref_start, ref_end from first and last anchor.
 */
void set_region_coordinates(
    AlignRegion& region,
    std::span<const Anchor> anchors,
    sequence_size_t query_len)
{
    if (region.seed_count <= 0) {
        return;
    }
    const int32_t anchor_begin = region.anchor_start;
    const Anchor& first_anchor = anchors[anchor_begin];
    const Anchor& last_anchor = anchors[anchor_begin + region.seed_count - 1];

    region.is_reverse = first_anchor.is_reverse != 0;
    region.reference_id = 0;  // DOC0263: single-target codebase, reference_id always 0

    // Reference coordinates. The absolute positions are full 64-bit; the
    // intermediate `position + 1 - span` can be negative, so it is computed in
    // signed int64 and clamped before being stored in the sequence_size_t field.
    const std::int64_t first_span = static_cast<int32_t>(first_anchor.query_span);
    std::int64_t ref_start = static_cast<std::int64_t>(first_anchor.ref_position) + 1 - first_span;
    const std::int64_t ref_end = static_cast<std::int64_t>(last_anchor.ref_position) + 1;
    if (ref_start < 0) {
        ref_start = 0;
    }
    region.ref_start = static_cast<sequence_size_t>(ref_start);
    region.ref_end = static_cast<sequence_size_t>(ref_end);

    // Query coordinates
    std::int64_t raw_query_start = static_cast<std::int64_t>(first_anchor.query_position)
                                 + 1 - first_span;
    const std::int64_t raw_query_end = static_cast<std::int64_t>(last_anchor.query_position) + 1;
    if (raw_query_start < 0) {
        raw_query_start = 0;
    }

    if (!region.is_reverse) {
        region.query_start = static_cast<sequence_size_t>(raw_query_start);
        region.query_end = static_cast<sequence_size_t>(raw_query_end);
    } else {
        const std::int64_t qlen = static_cast<std::int64_t>(query_len);
        region.query_start = static_cast<sequence_size_t>(qlen - raw_query_end);
        region.query_end = static_cast<sequence_size_t>(qlen - raw_query_start);
    }

    compute_fuzzy_lengths(region, anchors);
}

/**
 * @brief Convert chains to alignment regions.
 *
 * Equivalent to mm_gen_regs() in hit.c:54-90.
 *
 * Sorts chains by score (descending) and creates AlignRegion records
 * with coordinates derived from the anchor positions.
 *
 * @param chains       Chains (sorted in place by score descending here).
 * @param anchors      Reordered anchor span — `chains[i].anchor_start`
 *                      indexes into this span.
 * @param query_len    Length of the query sequence (used to flip query
 *                      coordinates for reverse-strand chains).
 * @param regions_out  Output slab — one AlignRegion per chain is constructed
 *                      via placement-new (so the slab's destructor releases
 *                      `region.cigar` correctly on unwind).  Must be a fresh
 *                      ArenaSlab; the kernel calls `reserve_for_placement_new`
 *                      and `construct_at_back` once per chain (DOC0249 §13
 *                      follow-up: output-side kernel-to-arena refactor).
 */
void generate_regions(
    std::span<Chain> chains,
    std::span<const Anchor> anchors,
    sequence_size_t query_len,
    ::bioseq::chaining::ArenaSlab<AlignRegion>& regions_out)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_gen_regs");
    auto _scope = bioseq::instr::region(_desc);
    if (chains.empty()) {
        return;
    }

    /// Chain comparator — orders chains by score descending so the highest-
    /// scoring chain becomes the primary alignment region.  Non-capturing
    /// functor for DOC0249 R4 compliance (no capture-all lambdas in the
    /// pipeline module tree).
    struct ChainScoreDescending {
        [[nodiscard, gnu::always_inline]] inline bool operator()(
            const Chain& left, const Chain& right) const noexcept
        {
            return left.score > right.score;
        }
    };
    std::sort(chains.begin(), chains.end(), ChainScoreDescending{});

    regions_out.reserve_for_placement_new(chains.size());
    for (size_t region_idx = 0; region_idx < chains.size(); ++region_idx) {
        AlignRegion& region = regions_out.construct_at_back(AlignRegion{
            .region_id      = static_cast<int32_t>(region_idx),
            .parent         = -1,
            .score          = chains[region_idx].score,
            .original_score = chains[region_idx].score,
            .seed_count     = chains[region_idx].anchor_count,
            .anchor_start   = chains[region_idx].anchor_start,
            .divergence     = -1.0f,
        });
        set_region_coordinates(region, anchors, query_len);
    }

    bioseq_log_lazy<DBG>([&] {
        bioseq_log_progress<DBG>("[minimap_port] Generated {} alignment regions | footprint={}MB avail={}MB",
                regions_out.size(),
                bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
    });
}

// ============================================================================
// Phase 5: Primary/secondary classification (mm_set_parent equivalent)
// ============================================================================

/**
 * @brief Classify alignments as primary or secondary.
 *
 * Equivalent to mm_set_parent() in hit.c:127-188.
 *
 * An alignment is secondary if it overlaps significantly with a primary
 * on the query AND has insufficient unique (uncovered) sequence.
 *
 * Criterion: (overlap/min_interval) - (uncov/max_interval) > mask_level
 *            AND uncov <= mask_len
 */
void classify_primary_secondary(
    std::span<AlignRegion> regions,
    float mask_level,
    int32_t mask_len,
    int32_t sub_diff)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_set_parent");
    auto _scope = bioseq::instr::region(_desc);
    const size_t region_count = regions.size();
    if (region_count == 0) {
        return;
    }

    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        regions[region_idx].region_id = static_cast<int32_t>(region_idx);
    }

    // primary_indices holds indices of primary alignments. Reserve up
    // front so the first push_back doesn't trigger a 1→2→4→… growth
    // chain through `region_count` worth of pushes when the chains
    // happen to be all primary. Sized at region_count (the upper bound
    // — every region is primary in the degenerate case).
    std::vector<size_t> primary_indices;
    primary_indices.reserve(region_count);
    primary_indices.push_back(0);
    regions[0].parent = 0;

    // coverage_intervals is cleared+refilled on every secondary check,
    // capped at primary_indices.size() entries. Pre-reserve at
    // region_count too so the inner loop never triggers realloc.
    std::vector<uint64_t> coverage_intervals;
    coverage_intervals.reserve(region_count);

    for (size_t region_idx = 1; region_idx < region_count; ++region_idx) {
        AlignRegion& current_region = regions[region_idx];
        const int32_t current_query_start = current_region.query_start;
        const int32_t current_query_end = current_region.query_end;
        int32_t uncovered_length = 0;

        // Compute coverage from existing primaries
        coverage_intervals.clear();
        for (const size_t primary_idx : primary_indices) {
            const AlignRegion& primary_region = regions[primary_idx];
            int32_t primary_query_start = primary_region.query_start;
            int32_t primary_query_end = primary_region.query_end;
            if (primary_query_end <= current_query_start
                || primary_query_start >= current_query_end)
            {
                continue;
            }
            primary_query_start = std::max(primary_query_start, current_query_start);
            primary_query_end = std::min(primary_query_end, current_query_end);
            coverage_intervals.push_back(
                (static_cast<uint64_t>(primary_query_start) << 32)
                | static_cast<uint32_t>(primary_query_end));
        }

        if (!coverage_intervals.empty()) {
            std::sort(coverage_intervals.begin(), coverage_intervals.end());
            int32_t coverage_pos = current_query_start;
            for (const uint64_t interval : coverage_intervals) {
                const int32_t interval_start = static_cast<int32_t>(interval >> 32);
                const int32_t interval_end = static_cast<int32_t>(interval);
                if (interval_start > coverage_pos) {
                    uncovered_length += interval_start - coverage_pos;
                }
                coverage_pos = std::max(coverage_pos, interval_end);
            }
            if (current_query_end > coverage_pos) {
                uncovered_length += current_query_end - coverage_pos;
            }
        }

        // Test against each primary
        bool is_secondary = false;
        for (const size_t primary_idx : primary_indices) {
            AlignRegion& primary_region = regions[primary_idx];
            const int32_t primary_query_start = primary_region.query_start;
            const int32_t primary_query_end = primary_region.query_end;
            if (primary_query_end <= current_query_start
                || primary_query_start >= current_query_end)
            {
                continue;
            }

            const int32_t min_span = std::min(primary_query_end - primary_query_start,
                                              current_query_end - current_query_start);
            const int32_t max_span = std::max(primary_query_end - primary_query_start,
                                              current_query_end - current_query_start);
            const int32_t overlap_length = std::max(0,
                std::min(current_query_end, primary_query_end)
                - std::max(current_query_start, primary_query_start));

            if (min_span > 0 && max_span > 0 &&
                (static_cast<float>(overlap_length) / min_span -
                 static_cast<float>(uncovered_length) / max_span) > mask_level &&
                uncovered_length <= mask_len)
            {
                const int32_t resolved_parent = (primary_region.parent >= 0)
                    ? primary_region.parent : primary_region.region_id;
                current_region.parent = resolved_parent;
                primary_region.secondary_score = std::max(
                    primary_region.secondary_score, current_region.score);
                // Update secondary_dp_max for MAPQ (equivalent to hit.c:176)
                if (current_region.has_alignment && primary_region.has_alignment &&
                    (primary_region.reference_id != current_region.reference_id
                     || primary_region.ref_start != current_region.ref_start
                     || primary_region.ref_end != current_region.ref_end
                     || overlap_length != min_span))
                {
                    primary_region.secondary_dp_max = std::max(
                        primary_region.secondary_dp_max, current_region.dp_max_score);
                    if (primary_region.dp_max_score - current_region.dp_max_score <= sub_diff) {
                        ++primary_region.secondary_count;
                    }
                } else if (current_region.seed_count >= primary_region.seed_count) {
                    ++primary_region.secondary_count;
                }
                is_secondary = true;
                break;
            }
        }

        if (!is_secondary) {
            primary_indices.push_back(region_idx);
            current_region.parent = static_cast<int32_t>(region_idx);
            current_region.secondary_count = 0;
        }
    }

    bioseq_log<DBG>("[minimap_port] Classification: {} primary, {} secondary",
            primary_indices.size(), region_count - primary_indices.size());
}

// ============================================================================
// Phase 6: Filter secondary mappings (mm_select_sub equivalent)
// ============================================================================

/**
 * @brief Filter secondary alignments by score ratio.
 *
 * Equivalent to mm_select_sub() in hit.c:257-277.
 *
 * Keep a secondary if:
 *   - score >= parent.score * pri_ratio, OR
 *   - score + min_diff >= parent.score
 *   - up to best_n secondaries total
 */
[[nodiscard]] std::size_t filter_secondary_regions(
    std::span<AlignRegion> regions,
    float pri_ratio,
    int32_t min_diff,
    int32_t best_n)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_select_sub");
    auto _scope = bioseq::instr::region(_desc);
    if (regions.empty() || pri_ratio <= 0.0f) {
        return regions.size();
    }

    // Use index-based filtering to avoid reading from moved-from objects.
    // Mark each region as kept (true) or removed (false), then compact.
    const size_t region_count = regions.size();
    size_t secondary_kept_count = 0;
    std::vector<bool> keep(region_count, false);

    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        const auto& region = regions[region_idx];
        const int32_t parent_idx = region.parent;
        if (parent_idx == region.region_id) {
            // Primary — always keep
            keep[region_idx] = true;
        } else if (parent_idx >= 0 && parent_idx < static_cast<int32_t>(region_count)) {
            const auto& parent_region = regions[parent_idx];
            if ((region.score >= static_cast<int32_t>(parent_region.score * pri_ratio) ||
                 region.score + min_diff >= parent_region.score)
                && static_cast<int32_t>(secondary_kept_count) < best_n)
            {
                // Not identical coordinates
                const bool is_duplicate =
                    region.query_start == parent_region.query_start
                    && region.query_end == parent_region.query_end
                    && region.reference_id == parent_region.reference_id
                    && region.ref_start == parent_region.ref_start
                    && region.ref_end == parent_region.ref_end;
                if (!is_duplicate) {
                    keep[region_idx] = true;
                    ++secondary_kept_count;
                }
            }
        }
    }

    // Compact: move kept regions to front.  Span has no resize(); the caller
    // observes the new effective size via the returned value.  Regions past
    // `write_pos` are moved-from (empty CIGAR vector) — their destructors run
    // when the owning storage (arena slab or vector) unwinds.
    size_t write_pos = 0;
    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        if (keep[region_idx]) {
            if (write_pos < region_idx) {
                regions[write_pos] = std::move(regions[region_idx]);
            }
            ++write_pos;
        }
    }

    // Re-index the surviving prefix.
    for (size_t region_idx = 0; region_idx < write_pos; ++region_idx) {
        regions[region_idx].region_id = static_cast<int32_t>(region_idx);
    }

    bioseq_log<DBG>("[minimap_port] After secondary filtering: {} regions remain", write_pos);
    return write_pos;
}

// ============================================================================
// Phase 7: Base-level alignment — dual-affine gap DP (KSW2 equivalent)
// ============================================================================

// generate_score_matrix is now ScoringMatrix<R>::generate in alignment_hwy_utils.

// ============================================================================
// KSW2 alignment wrapper
// ============================================================================

/// Operation-local evidence emitted only by the scalar minimizer entry point.
/// The selected enum is deliberately not part of this value: a caller receives
/// evidence only after the scalar collector returned successfully.
struct ScalarMinimizerExecutionProof {
    std::uint64_t calls = 0;
    std::uint64_t input_symbols = 0;
    bool saturated = false;

    static constexpr void add(
            std::uint64_t& destination,
            const std::uint64_t increment,
            bool& saturated_flag) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                            - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturated_flag = true;
            return;
        }
        destination += increment;
    }

    constexpr void record(const std::uint64_t symbols) noexcept {
        add(calls, 1u, saturated);
        add(input_symbols, symbols, saturated);
    }

    constexpr void combine(
            const ScalarMinimizerExecutionProof& other) noexcept {
        bool overflow = saturated || other.saturated;
        add(calls, other.calls, overflow);
        add(input_symbols, other.input_symbols, overflow);
        saturated = overflow;
    }

    [[nodiscard]] constexpr bool accepted(
            const bool require_nonempty_work = true) const noexcept {
        return !saturated
            && calls > 0
            && (!require_nonempty_work || input_symbols > 0);
    }

    constexpr bool operator==(
        const ScalarMinimizerExecutionProof&) const noexcept = default;
};

enum class ScalarKsw2Phase : std::uint8_t {
    left,
    gap,
    right,
};

/// Positive, operation-originated proof for scalar CPU dimensions in a route:
/// scalar minimizers, CPU seed lookup, scalar chaining/backtracking, and KSW2
/// extension. Hybrid routes may populate only the scalar dimensions they
/// execute; SIMD/Block/Vulkan proof objects remain separate.
struct ScalarPipelineExecutionProof {
    ScalarMinimizerExecutionProof query_minimizers{};
    ScalarMinimizerExecutionProof target_minimizers{};
    std::uint64_t seed_lookup_calls = 0;
    std::uint64_t seed_query_minimizers = 0;
    std::uint64_t chain_score_calls = 0;
    std::uint64_t chain_backtrack_calls = 0;
    std::uint64_t chain_anchor_inputs = 0;
    std::uint64_t ksw2_left_calls = 0;
    std::uint64_t ksw2_gap_calls = 0;
    std::uint64_t ksw2_right_calls = 0;
    // Rectangle area admitted to real KSW2 calls. This is not a claim that a
    // banded/z-drop kernel visited every cell; it proves positive DP input.
    std::uint64_t ksw2_input_cell_area = 0;
    bool saturated = false;

    static constexpr void add(
            std::uint64_t& destination,
            const std::uint64_t increment,
            bool& saturated_flag) noexcept {
        ScalarMinimizerExecutionProof::add(
            destination, increment, saturated_flag);
    }

    constexpr void record_seed_lookup(
            const std::uint64_t query_minimizer_count) noexcept {
        add(seed_lookup_calls, 1u, saturated);
        add(seed_query_minimizers, query_minimizer_count, saturated);
    }

    constexpr void record_scalar_chain(
            const std::uint64_t anchor_count) noexcept {
        add(chain_score_calls, 1u, saturated);
        add(chain_backtrack_calls, 1u, saturated);
        add(chain_anchor_inputs, anchor_count, saturated);
    }

    constexpr void record_ksw2_call(
            const ScalarKsw2Phase phase,
            const std::uint64_t query_symbols,
            const std::uint64_t target_symbols) noexcept {
        switch (phase) {
            case ScalarKsw2Phase::left:
                add(ksw2_left_calls, 1u, saturated);
                break;
            case ScalarKsw2Phase::gap:
                add(ksw2_gap_calls, 1u, saturated);
                break;
            case ScalarKsw2Phase::right:
                add(ksw2_right_calls, 1u, saturated);
                break;
        }
        if (query_symbols != 0
            && target_symbols
                > (std::numeric_limits<std::uint64_t>::max)()
                    / query_symbols) {
            ksw2_input_cell_area =
                (std::numeric_limits<std::uint64_t>::max)();
            saturated = true;
            return;
        }
        add(ksw2_input_cell_area,
            query_symbols * target_symbols, saturated);
    }

    constexpr void combine(
            const ScalarPipelineExecutionProof& other) noexcept {
        query_minimizers.combine(other.query_minimizers);
        target_minimizers.combine(other.target_minimizers);
        bool overflow = saturated || other.saturated
            || query_minimizers.saturated
            || target_minimizers.saturated;
        add(seed_lookup_calls, other.seed_lookup_calls, overflow);
        add(seed_query_minimizers, other.seed_query_minimizers, overflow);
        add(chain_score_calls, other.chain_score_calls, overflow);
        add(chain_backtrack_calls, other.chain_backtrack_calls, overflow);
        add(chain_anchor_inputs, other.chain_anchor_inputs, overflow);
        add(ksw2_left_calls, other.ksw2_left_calls, overflow);
        add(ksw2_gap_calls, other.ksw2_gap_calls, overflow);
        add(ksw2_right_calls, other.ksw2_right_calls, overflow);
        add(ksw2_input_cell_area, other.ksw2_input_cell_area, overflow);
        saturated = overflow;
    }

    /// Validate the complete all-scalar tuple. Hybrid routes validate only the
    /// scalar dimensions they selected at their route-proof boundary.
    [[nodiscard]] constexpr bool accepted(
            const bool require_target_sketch,
            const bool require_alignment) const noexcept {
        const bool target_valid = require_target_sketch
            ? target_minimizers.accepted()
            : target_minimizers == ScalarMinimizerExecutionProof{};
        const bool alignment_valid = !require_alignment
            || ((ksw2_left_calls != 0
                 || ksw2_gap_calls != 0
                 || ksw2_right_calls != 0)
                && ksw2_input_cell_area > 0);
        return !saturated
            && query_minimizers.accepted()
            && target_valid
            && seed_lookup_calls > 0
            && seed_query_minimizers > 0
            && chain_score_calls > 0
            && chain_backtrack_calls > 0
            && chain_anchor_inputs > 0
            && alignment_valid;
    }

    constexpr bool operator==(
        const ScalarPipelineExecutionProof&) const noexcept = default;
};

/// Operation-local evidence for the selected KSW2_BATCHED alignment stage.
/// The proof is populated by the real fill collector and bucket executor; it
/// never infers execution from the enum selected by the caller.  The current
/// implementation deliberately calls the counters "short bucket" rather than
/// "SIMD" because short fills still execute the KSW2 kernel lane by lane.
struct Ksw2BatchedExecutionProof {
    std::uint64_t align_region_invocations = 0;
    std::uint64_t regions_seen = 0;
    std::uint64_t fills_collected = 0;
    std::uint64_t nonempty_fill_calls = 0;
    std::uint64_t short_bucket_dispatches = 0;
    std::uint64_t short_bucket_lanes = 0;
    std::uint64_t long_fill_calls = 0;
    std::uint64_t input_cell_area = 0;
    bool saturated = false;

    static constexpr void add(
            std::uint64_t& destination,
            const std::uint64_t increment,
            bool& saturated_flag) noexcept {
        ScalarMinimizerExecutionProof::add(
            destination, increment, saturated_flag);
    }

    constexpr void record_invocation(
            const std::uint64_t region_count) noexcept {
        add(align_region_invocations, 1u, saturated);
        add(regions_seen, region_count, saturated);
    }

    constexpr void record_collected_fills(
            const std::uint64_t fill_count) noexcept {
        add(fills_collected, fill_count, saturated);
    }

    constexpr void record_short_bucket_dispatch(
            const std::uint64_t lane_count) noexcept {
        add(short_bucket_dispatches, 1u, saturated);
        add(short_bucket_lanes, lane_count, saturated);
    }

    constexpr void record_nonempty_fill(
            const std::uint64_t query_symbols,
            const std::uint64_t target_symbols,
            const bool long_fill) noexcept {
        add(nonempty_fill_calls, 1u, saturated);
        if (long_fill) {
            add(long_fill_calls, 1u, saturated);
        }
        if (query_symbols != 0u
            && target_symbols
                > (std::numeric_limits<std::uint64_t>::max)()
                    / query_symbols) {
            input_cell_area =
                (std::numeric_limits<std::uint64_t>::max)();
            saturated = true;
            return;
        }
        add(input_cell_area,
            query_symbols * target_symbols, saturated);
    }

    constexpr void combine(
            const Ksw2BatchedExecutionProof& other) noexcept {
        bool overflow = saturated || other.saturated;
        add(align_region_invocations,
            other.align_region_invocations, overflow);
        add(regions_seen, other.regions_seen, overflow);
        add(fills_collected, other.fills_collected, overflow);
        add(nonempty_fill_calls, other.nonempty_fill_calls, overflow);
        add(short_bucket_dispatches,
            other.short_bucket_dispatches, overflow);
        add(short_bucket_lanes, other.short_bucket_lanes, overflow);
        add(long_fill_calls, other.long_fill_calls, overflow);
        add(input_cell_area, other.input_cell_area, overflow);
        saturated = overflow;
    }

    [[nodiscard]] constexpr bool accepted(
            const bool require_nonempty_work) const noexcept {
        const bool short_route_coherent =
            (short_bucket_dispatches == 0u)
                == (short_bucket_lanes == 0u);
        const bool calls_coherent = short_bucket_lanes
                <= (std::numeric_limits<std::uint64_t>::max)()
                    - long_fill_calls
            && nonempty_fill_calls
                == short_bucket_lanes + long_fill_calls
            && short_bucket_dispatches <= short_bucket_lanes
            && nonempty_fill_calls == fills_collected
            && (nonempty_fill_calls == 0u)
                == (input_cell_area == 0u)
            && input_cell_area >= nonempty_fill_calls;
        return !saturated
            && align_region_invocations > 0u
            && short_route_coherent
            && calls_coherent
            && (!require_nonempty_work
                || (regions_seen > 0u
                    && fills_collected > 0u
                    && nonempty_fill_calls > 0u));
    }

    constexpr bool operator==(
        const Ksw2BatchedExecutionProof&) const noexcept = default;
};

static_assert([] {
    Ksw2BatchedExecutionProof proof{};
    proof.record_invocation(1u);
    proof.record_collected_fills(2u);
    proof.record_short_bucket_dispatch(1u);
    proof.record_nonempty_fill(8u, 9u, false);
    proof.record_nonempty_fill(300u, 400u, true);
    return proof.accepted(true)
        && proof.nonempty_fill_calls == 2u
        && proof.input_cell_area == 120'072u;
}());

/**
 * @brief Dispatch to KSW2's dual-affine or affine gap alignment.
 *
 * Equivalent to mm_align_pair() in align.c:340-372.
 * If q == q2 && e == e2, uses affine gap (z2); otherwise dual-affine (d2).
 *
 * Accepts compact_sequence_range directly with sub-region indices so that
 * sequence encoding happens only inside the KSW2 core kernel.
 *
 * @tparam is_reversed  True for reverse-complement query strand
 * @tparam QueryRange   compact_sequence_range type for query
 * @tparam TargetRange  compact_sequence_range type for target
 * @tparam AlphabetDim  Scoring matrix dimension (default: alphabet_size + 1)
 */
[[nodiscard]] constexpr bool block_aligner_native_penalties_representable(
        const Minimap2Params& params) noexcept {
    return params.gap_open1 > 0
        && params.gap_ext1 > 0
        && params.gap_open1 <= std::numeric_limits<int8_t>::max()
        && params.gap_ext1 <= std::numeric_limits<int8_t>::max()
        && params.gap_open1 + params.gap_ext1
            <= -static_cast<int>(std::numeric_limits<int8_t>::min());
}

[[nodiscard]] inline BlockAlignerParams project_block_aligner_params(
        const Minimap2Params& params,
        BlockAlignerPhase phase) noexcept {
    const bool is_extension = phase != BlockAlignerPhase::GapExtension;
    return BlockAlignerParams{
        .gap_open   = static_cast<int8_t>(params.gap_open1),
        .gap_extend = static_cast<int8_t>(params.gap_ext1),
        .min_size   = params.block_min_size,
        .max_size   = params.block_max_size,
        .x_drop     = is_extension ? params.zdrop : 0,
        .mode       = is_extension
            ? BlockAlignmentMode::XDrop
            : BlockAlignmentMode::Global,
        .input_order = phase == BlockAlignerPhase::LeftExtension
            ? BlockInputOrder::Reversed
            : BlockInputOrder::Forward,
        .phase      = phase,
        .trace      = params.do_cigar || params.compute_edit_distance,
        .compute_edit_distance = params.compute_edit_distance,
    };
}

template<
    bool is_reversed,
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
[[nodiscard]] KswAlignResult ksw2_align_pair(
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const ScoringMatrix<QueryRange>& scoring_matrix,
    const Minimap2Params& params,
    int align_bandwidth, int end_bonus, int zdrop, KswFlags ksw_flags,
    BlockAlignerExecutionProof* forbidden_block_proof = nullptr,
    ScalarPipelineExecutionProof* scalar_execution_proof = nullptr,
    ScalarKsw2Phase scalar_phase = ScalarKsw2Phase::gap)
{
    if (forbidden_block_proof != nullptr) {
        block_aligner_forbidden_ksw2_fallback(*forbidden_block_proof);
    }
    if (scalar_execution_proof != nullptr) {
        scalar_execution_proof->record_ksw2_call(
            scalar_phase,
            static_cast<std::uint64_t>(query_end - query_start),
            static_cast<std::uint64_t>(target_end - target_start));
    }
    if (params.gap_open1 == params.gap_open2 && params.gap_ext1 == params.gap_ext2) {
        const KswZ2Params z2_params{
            .gap_open   = static_cast<int8_t>(params.gap_open1),
            .gap_extend = static_cast<int8_t>(params.gap_ext1),
            .band_width = align_bandwidth,
            .z_drop     = zdrop,
            .end_bonus  = end_bonus,
            .flags      = ksw_flags,
        };
        return HwyKsw2Aligner::z2_align<is_reversed>(
            query_seq, target_seq,
            query_start, query_end, target_start, target_end,
            scoring_matrix, z2_params);
    }
    const KswD2Params d2_params{
        .gap_open_short   = static_cast<int8_t>(params.gap_open1),
        .gap_extend_short = static_cast<int8_t>(params.gap_ext1),
        .gap_open_long    = static_cast<int8_t>(params.gap_open2),
        .gap_extend_long  = static_cast<int8_t>(params.gap_ext2),
        .band_width       = align_bandwidth,
        .z_drop           = zdrop,
        .end_bonus        = end_bonus,
        .flags            = ksw_flags,
    };
    return HwyKsw2Aligner::d2_align<is_reversed>(
        query_seq, target_seq,
        query_start, query_end, target_start, target_end,
        scoring_matrix, d2_params);
}

/**
 * @brief Project the native Block Aligner contract for one minimap phase.
 *
 * This is deliberately not a KSW2-compatible adapter. The reference Block
 * Aligner has a single affine recurrence, adaptive block geometry, global
 * mode, and its own two-confirmation X-drop mode. Consequently only the
 * short affine pair and the Block tunables are projected from Minimap2Params:
 *
 *   - gap fill: reference Global mode, forward input;
 *   - left extension: reference XDrop mode, reversed input;
 *   - right extension: reference XDrop mode, forward input.
 *
 * Dormant KSW-only defaults in the shared parameter object (bandwidth,
 * right-align, end-bonus, the second affine pair, and approximate-max) do not
 * invalidate a native Block run and are not included in its proof identity.
 * They are intentionally absent from this function's call surface, so a
 * caller that claims those semantics gets a compile-time contract error
 * instead of a KSW2 or scalar fallback.
 */
template<
    bool is_reversed,
    bool enable_diagonal_certificate = true,
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
[[nodiscard]] KswAlignResult block_aligner_align_pair(
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const ScoringMatrix<QueryRange>& scoring_matrix,
    const Minimap2Params& params,
    BlockAlignerPhase phase,
    BlockAlignerExecutionProof* execution_proof)
{
    const bool is_extension = phase != BlockAlignerPhase::GapExtension;
    if (!block_aligner_native_penalties_representable(params)
        || (is_extension && params.zdrop < 0)) {
        if (execution_proof != nullptr) {
            execution_proof->add(
                execution_proof->unsupported_contract_rejections);
        }
        bioseq_log<ERR>(
            "Block Aligner native parameter projection rejected: q={}, e={}, "
            "zdrop={}, phase={}",
            params.gap_open1, params.gap_ext1, params.zdrop,
            static_cast<unsigned>(phase));
        std::abort();
    }
    const BlockAlignerParams ba_params = project_block_aligner_params(
        params, phase);
    return HwyBlockAligner::align<
        is_reversed, enable_diagonal_certificate>(
        query_seq, target_seq,
        query_start, query_end, target_start, target_end,
        scoring_matrix, ba_params, execution_proof);
}

// append_cigar is now in bioseq_cigar module.

/**
 * @brief Perform base-level alignment for each region.
 *
 * Equivalent to align_regs() -> mm_align_skeleton() -> mm_align1() in align.c.
 *
 * For each region, follows the minimap2 3-part alignment strategy:
 *   1. Left extension: reversed query/target from first seed to extended boundary
 *   2. Gap filling: align the entire region between first and last seeds
 *   3. Right extension: query/target from last seed to extended boundary
 *
 * Sequences are passed as compact_sequence_range references with sub-region
 * indices — no pre-encoding into uint8_t vectors is needed. The selected
 * base-alignment kernel encodes only the admitted sub-regions.
 *
 * @tparam base_alignment_algorithm  Compile-time selector between KSW2 and
 *                                   block aligner. Templated so the inner
 *                                   gap-fill branch is resolved at compile
 *                                   time and no runtime branch exists in the
 *                                   hot per-region loop.
 */
/**
 * @brief Single-phase aligner — left extension + gap fill + right extension.
 *
 * Materialises the three KSW2/block-aligner calls per region into a struct
 * (DOC0249 §10.1 R4: replace the `[&]<bool>()` capture-all template lambda
 * that previously lived inside `align_regions::for(auto& region : regions)`
 * with a non-capturing functor whose configuration is stored by value).
 * Inlines to the same instructions at -O3.
 *
 * `run<strand_reversed>` is templated on the strand because the alignment
 * kernels are themselves templated on the reverse-complement encoding bit.
 * Native Block left extension additionally emits reversed compact input and
 * reverses its traced operations back to source order.
 */
struct BlockPhaseEligibility {
    std::uint64_t left = 0;
    std::uint64_t gap = 0;
    std::uint64_t right = 0;
};

/// Geometry of one mandatory seed-to-seed global fill.  Left/right
/// extensions are deliberately excluded: they are local/X-drop domains and
/// therefore cannot support a no-false-reject edit-threshold prefilter.
struct MandatoryAlignmentGapWindow {
    sequence_size_t query_start = 0;
    sequence_size_t query_end = 0;
    sequence_size_t target_start = 0;
    sequence_size_t target_end = 0;
};

template <class Callback>
[[gnu::always_inline]] bool for_each_mandatory_alignment_gap(
    const AlignRegion& region,
    std::span<const Anchor> anchors,
    const Minimap2Params& params,
    Callback&& callback)
{
    if (region.seed_count <= 1 || region.anchor_start < 0
        || static_cast<std::size_t>(region.anchor_start)
            >= anchors.size()) {
        return true;
    }
    const std::int32_t anchor_begin = region.anchor_start;
    const Anchor& first_anchor = anchors[anchor_begin];
    const std::int64_t first_span =
        static_cast<std::int64_t>(first_anchor.query_span);
    std::int64_t query_start =
        static_cast<std::int64_t>(first_anchor.query_position) + 1 - first_span;
    std::int64_t target_start =
        static_cast<std::int64_t>(first_anchor.ref_position) + 1 - first_span;
    query_start = std::max<std::int64_t>(0, query_start);
    target_start = std::max<std::int64_t>(0, target_start);

    for (std::int32_t index = 1; index < region.seed_count; ++index) {
        const std::size_t anchor_index = static_cast<std::size_t>(
            anchor_begin + index);
        if (anchor_index >= anchors.size()) return false;
        const Anchor& current = anchors[anchor_index];
        const std::int64_t query_end =
            static_cast<std::int64_t>(current.query_position) + 1;
        const std::int64_t target_end =
            static_cast<std::int64_t>(current.ref_position) + 1;
        const bool is_last = index == region.seed_count - 1;
        const std::int64_t query_length = query_end - query_start;
        const std::int64_t target_length = target_end - target_start;
        if (!is_last
            && (query_length < params.min_ksw_len
                || target_length < params.min_ksw_len)) {
            continue;
        }
        if (query_length <= 0 || target_length <= 0) {
            query_start = query_end;
            target_start = target_end;
            continue;
        }
        const MandatoryAlignmentGapWindow window{
            .query_start = static_cast<sequence_size_t>(query_start),
            .query_end = static_cast<sequence_size_t>(query_end),
            .target_start = static_cast<sequence_size_t>(target_start),
            .target_end = static_cast<sequence_size_t>(target_end),
        };
        if (!callback(window)) return false;
        query_start = query_end;
        target_start = target_end;
    }
    return true;
}

/**
 * Compute the exact unit-cost edit distance represented by one packed CIGAR
 * and its already-clipped sequence windows.  Generic M operations must be
 * compared against the underlying alphabet ranks because the block aligner
 * deliberately projects both Eq and X to M at its public CIGAR boundary.
 */
template <bool strand_reversed,
          supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
[[nodiscard]] sequence_size_t exact_edit_distance_from_packed_cigar(
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    std::size_t query_start,
    std::size_t query_end,
    std::size_t target_start,
    std::size_t target_end,
    std::span<const std::uint32_t> cigar)
{
    if (query_end < query_start || target_end < target_start) {
        throw std::invalid_argument("exact edit-distance window is inverted");
    }
    const std::size_t query_length = query_end - query_start;
    const std::size_t target_length = target_end - target_start;
    std::vector<std::uint8_t> query_ranks(query_length);
    std::vector<std::uint8_t> target_ranks(target_length);
    query_seq.template to_encoded_into<strand_reversed, false>(
        std::span<std::uint8_t>(query_ranks), query_start, query_end);
    target_seq.template to_encoded_into<false, false>(
        std::span<std::uint8_t>(target_ranks), target_start, target_end);

    std::size_t query_position = 0;
    std::size_t target_position = 0;
    sequence_size_t edits = 0;
    for (const std::uint32_t packed : cigar) {
        const std::uint32_t opcode = packed & 0xfU;
        const std::size_t length = static_cast<std::size_t>(packed >> 4);
        const bool consumes_query = opcode == CIGAR_MATCH
            || opcode == CIGAR_EQ || opcode == CIGAR_X
            || opcode == CIGAR_INS;
        const bool consumes_target = opcode == CIGAR_MATCH
            || opcode == CIGAR_EQ || opcode == CIGAR_X
            || opcode == CIGAR_DEL || opcode == CIGAR_N_SKIP;
        if ((consumes_query
                && (query_position > query_length
                    || length > query_length - query_position))
            || (consumes_target
                && (target_position > target_length
                    || length > target_length - target_position))) {
            throw std::runtime_error(
                "alignment CIGAR consumes beyond its exact edit-distance window");
        }

        switch (opcode) {
            case CIGAR_MATCH:
                for (std::size_t offset = 0; offset < length; ++offset) {
                    edits += query_ranks[query_position + offset]
                        != target_ranks[target_position + offset];
                }
                break;
            case CIGAR_EQ:
                break;
            case CIGAR_X:
            case CIGAR_INS:
            case CIGAR_DEL:
            case CIGAR_N_SKIP:
                edits += static_cast<sequence_size_t>(length);
                break;
            default:
                throw std::runtime_error(
                    "alignment CIGAR contains an unsupported exact edit-distance opcode");
        }
        if (consumes_query) query_position += length;
        if (consumes_target) target_position += length;
    }
    if (query_position != query_length || target_position != target_length) {
        throw std::runtime_error(
            "alignment CIGAR does not cover its exact edit-distance window");
    }
    return edits;
}

/**
 * Count the implicit seed alignment of a single-seed region.
 *
 * Multi-seed regions include the first seed in their first mandatory gap
 * fill.  A single-seed region has no such fill, so the seed lies between the
 * independently aligned left and right extensions.  A minimizer hash is not
 * a proof that the underlying ranks are equal; exact edit-distance callers
 * must compare this span explicitly.
 */
template <bool strand_reversed,
          supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
[[nodiscard]] sequence_size_t exact_edit_distance_for_implicit_seed(
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    std::size_t query_start,
    std::size_t query_end,
    std::size_t target_start,
    std::size_t target_end)
{
    if (query_end < query_start || target_end < target_start) {
        throw std::invalid_argument("implicit seed edit-distance window is inverted");
    }
    if (query_end > query_seq.bp_size() || target_end > target_seq.bp_size()) {
        throw std::out_of_range(
            "implicit seed edit-distance window exceeds the sequence");
    }

    const std::size_t query_length = query_end - query_start;
    const std::size_t target_length = target_end - target_start;
    const std::size_t common_length = std::min(query_length, target_length);
    std::vector<std::uint8_t> query_ranks(common_length);
    std::vector<std::uint8_t> target_ranks(common_length);
    query_seq.template to_encoded_into<strand_reversed, false>(
        std::span<std::uint8_t>(query_ranks),
        query_start,
        query_start + common_length);
    target_seq.template to_encoded_into<false, false>(
        std::span<std::uint8_t>(target_ranks),
        target_start,
        target_start + common_length);

    sequence_size_t edits = static_cast<sequence_size_t>(
        query_length > target_length
            ? query_length - target_length
            : target_length - query_length);
    for (std::size_t offset = 0; offset < common_length; ++offset) {
        edits += query_ranks[offset] != target_ranks[offset];
    }
    return edits;
}

inline void append_implicit_seed_cigar(
    std::vector<CigarOp>& cigar,
    std::size_t query_length,
    std::size_t target_length)
{
    const auto append_op = [&cigar](std::uint32_t opcode, std::size_t length) {
        if (length == 0) return;
        if (!cigar.empty() && cigar.back().opcode == opcode) {
            cigar.back().length += static_cast<std::uint32_t>(length);
        } else {
            cigar.push_back(CigarOp{
                .length = static_cast<std::uint32_t>(length),
                .opcode = opcode,
            });
        }
    };

    const std::size_t common_length = std::min(query_length, target_length);
    append_op(CIGAR_MATCH, common_length);
    if (query_length > common_length) {
        append_op(CIGAR_INS, query_length - common_length);
    } else if (target_length > common_length) {
        append_op(CIGAR_DEL, target_length - common_length);
    }
}

/// Derive the complete native-Block phase contract before dispatch from only
/// immutable region geometry. These values are deliberately recorded outside
/// the call branches they attest, so deleting or bypassing a phase cannot
/// delete both the expectation and execution evidence.
[[nodiscard]] inline BlockPhaseEligibility derive_block_phase_eligibility(
        const AlignRegion& region,
        std::span<const Anchor> anchors,
        std::int32_t anchor_begin,
        std::int64_t seed_ref_start,
        std::int64_t seed_query_start,
        std::int64_t seed_ref_end,
        std::int64_t seed_query_end,
        std::int64_t outer_ref_start,
        std::int64_t outer_query_start,
        std::int64_t outer_ref_end,
        std::int64_t outer_query_end,
        const Minimap2Params& params) noexcept {
    BlockPhaseEligibility expected{
        .left = seed_query_start > outer_query_start
                && seed_ref_start > outer_ref_start ? 1u : 0u,
        .gap = 0,
        .right = seed_query_end < outer_query_end
                 && seed_ref_end < outer_ref_end ? 1u : 0u,
    };
    if (region.seed_count <= 1) return expected;

    std::int64_t qs = seed_query_start;
    std::int64_t rs = seed_ref_start;
    for (std::int32_t index = 1; index < region.seed_count; ++index) {
        const Anchor& current = anchors[anchor_begin + index];
        const std::int64_t qe =
            static_cast<std::int64_t>(current.query_position) + 1;
        const std::int64_t re =
            static_cast<std::int64_t>(current.ref_position) + 1;
        const bool is_last = index == region.seed_count - 1;
        const std::int64_t gap_query = qe - qs;
        const std::int64_t gap_ref = re - rs;
        if (!is_last
            && (gap_query < params.min_ksw_len
                || gap_ref < params.min_ksw_len)) {
            continue;
        }
        if (gap_query <= 0 || gap_ref <= 0) {
            qs = qe;
            rs = re;
            continue;
        }
        ++expected.gap;
        qs = qe;
        rs = re;
    }
    return expected;
}

template <BaseAlignmentAlgorithmType base_alignment_algorithm,
          supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>,
          bool enable_diagonal_certificate = true>
struct AlignOnePhase {
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>&  query_seq;
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq;
    const ScoringMatrix<QueryRange>&           scoring_matrix;
    const Minimap2Params&                      params;
    int32_t                                    align_bandwidth;

    /**
     * Return exact edit accounting for one completed fill.
     *
     * Native Block traceback counts edits while its already-expanded padded
     * query/target ranks are resident. Other kernels retain the established
     * post-result expansion path and therefore keep identical semantics.
     */
    template <bool strand_reversed>
    [[gnu::always_inline]] sequence_size_t exact_edit_distance_for_fill(
            const KswAlignResult& fill_result,
            std::size_t query_start,
            std::size_t query_end,
            std::size_t target_start,
            std::size_t target_end) const {
        if constexpr (base_alignment_algorithm
                == BaseAlignmentAlgorithmType::BLOCK_ALIGNER) {
            if (!fill_result.edit_distance_valid) {
                bioseq_log<ERR>(
                    "Block Aligner result omitted requested exact edit accounting");
                std::abort();
            }
            return static_cast<sequence_size_t>(fill_result.edit_distance);
        } else {
            return exact_edit_distance_from_packed_cigar<strand_reversed>(
                query_seq, target_seq,
                query_start, query_end, target_start, target_end,
                std::span<const std::uint32_t>(fill_result.cigar));
        }
    }

    template <bool strand_reversed>
    [[gnu::always_inline]] bool apply_gap_fill_result(
            AlignRegion& region,
            const KswAlignResult& fill_result,
            std::int64_t qe,
            std::int64_t re,
            std::int64_t& qs,
            std::int64_t& rs,
            std::int64_t& final_query_end,
            std::int64_t& final_ref_end) const {
        if (!fill_result.cigar.empty()) {
            append_cigar(region.cigar, fill_result.cigar);
        }
        const std::int64_t fill_ref_end = fill_result.zdropped
            ? rs + (fill_result.max_t + 1) : re;
        const std::int64_t fill_query_end = fill_result.zdropped
            ? qs + (fill_result.max_q + 1) : qe;
        if (params.compute_edit_distance) {
            region.edit_distance += exact_edit_distance_for_fill<
                strand_reversed>(
                    fill_result,
                    static_cast<std::size_t>(qs),
                    static_cast<std::size_t>(fill_query_end),
                    static_cast<std::size_t>(rs),
                    static_cast<std::size_t>(fill_ref_end));
        }
        if (fill_result.zdropped) {
            final_ref_end = fill_ref_end;
            final_query_end = fill_query_end;
            region.dp_score += fill_result.max;
            return true;
        }
        final_ref_end = re;
        final_query_end = qe;
        region.dp_score += fill_result.score;
        qs = qe;
        rs = re;
        return false;
    }

    template <bool strand_reversed>
    [[gnu::always_inline]] void run(
        AlignRegion&            region,
        std::span<const Anchor> anchors,
        int32_t                 anchor_begin,
        std::int64_t            seed_ref_start,
        std::int64_t            seed_query_start,
        std::int64_t            seed_ref_end,
        std::int64_t            seed_query_end,
        std::int64_t            outer_ref_start,
        std::int64_t            outer_query_start,
        std::int64_t            outer_ref_end,
        std::int64_t            outer_query_end,
        std::int64_t            query_length,
        BlockAlignerExecutionProof* block_execution_proof,
        ScalarPipelineExecutionProof* scalar_execution_proof) const
    {
        bool prior_phase_zdropped = false;

        // === 1. Left extension ===
        std::int64_t final_ref_start = seed_ref_start;
        std::int64_t final_query_start = seed_query_start;
        if (seed_query_start > outer_query_start && seed_ref_start > outer_ref_start) {
            KswAlignResult left_result;
            if constexpr (base_alignment_algorithm
                    == BaseAlignmentAlgorithmType::BLOCK_ALIGNER) {
                left_result = block_aligner_align_pair<strand_reversed>(
                    query_seq, target_seq,
                    static_cast<size_t>(outer_query_start),
                    static_cast<size_t>(seed_query_start),
                    static_cast<size_t>(outer_ref_start),
                    static_cast<size_t>(seed_ref_start),
                    scoring_matrix, params,
                    BlockAlignerPhase::LeftExtension,
                    block_execution_proof);
            } else {
                left_result = ksw2_align_pair<strand_reversed>(
                    query_seq, target_seq,
                    static_cast<size_t>(outer_query_start),
                    static_cast<size_t>(seed_query_start),
                    static_cast<size_t>(outer_ref_start),
                    static_cast<size_t>(seed_ref_start),
                    scoring_matrix, params,
                    align_bandwidth, params.end_bonus, params.zdrop,
                    KswFlags{.right_align = true, .extz_only = true,
                             .reverse_cigar = true},
                    block_execution_proof,
                    scalar_execution_proof,
                    ScalarKsw2Phase::left);
            }
            if (!left_result.cigar.empty()) {
                append_cigar(region.cigar, left_result.cigar);
                region.dp_score += left_result.max;
            }
            const int32_t left_ref_offset = left_result.reach_end
                ? left_result.mqe_t + 1 : left_result.max_t + 1;
            final_ref_start = seed_ref_start - left_ref_offset;
            const int32_t left_query_offset = left_result.reach_end
                ? seed_query_start - outer_query_start : left_result.max_q + 1;
            final_query_start = seed_query_start - left_query_offset;
            if (params.compute_edit_distance) {
                region.edit_distance += exact_edit_distance_for_fill<
                    strand_reversed>(
                        left_result,
                        static_cast<std::size_t>(final_query_start),
                        static_cast<std::size_t>(seed_query_start),
                        static_cast<std::size_t>(final_ref_start),
                        static_cast<std::size_t>(seed_ref_start));
            }
            prior_phase_zdropped = left_result.zdropped;
        }

        // === 2. Gap filling — per-seed-pair walk ===
        std::int64_t final_ref_end = seed_ref_start;
        std::int64_t final_query_end = seed_query_start;
        if (region.seed_count > 1) {
            std::int64_t qs = seed_query_start;
            std::int64_t rs = seed_ref_start;
            bool zdropped_any = false;
            BlockGlobalGapCertificatePlan packed_gap_plan{};
            bool packed_gap_plan_ready = false;
            final_ref_end = seed_ref_start;
            final_query_end = seed_query_start;
            for (int32_t i = 1; i < region.seed_count; ++i) {
                const Anchor& cur_anchor = anchors[anchor_begin + i];
                const std::int64_t qe = static_cast<std::int64_t>(cur_anchor.query_position) + 1;
                const std::int64_t re = static_cast<std::int64_t>(cur_anchor.ref_position) + 1;
                const bool is_last = (i == region.seed_count - 1);
                const std::int64_t gap_query = qe - qs;
                const std::int64_t gap_ref   = re - rs;
                if (!is_last
                    && (gap_query < params.min_ksw_len
                        || gap_ref < params.min_ksw_len))
                {
                    continue;
                }
                if (gap_query <= 0 || gap_ref <= 0) {
                    qs = qe;
                    rs = re;
                    continue;
                }
                if constexpr (base_alignment_algorithm == BaseAlignmentAlgorithmType::BLOCK_ALIGNER) {
                    BlockPackedCertificateAttempt packed_attempt{};
                    if constexpr (enable_diagonal_certificate) {
                        // Prepare the immutable matrix/contract state only
                        // after the anchor walk produces its first valid gap.
                        // Invalid minimap penalty projections remain on the
                        // existing adapter path, which records and aborts.
                        if (block_aligner_native_penalties_representable(
                                params)) {
                            if (!packed_gap_plan_ready) {
                                packed_gap_plan =
                                    HwyBlockAligner::prepare_global_gap_certificate(
                                        scoring_matrix,
                                        project_block_aligner_params(
                                            params,
                                            BlockAlignerPhase::GapExtension));
                                packed_gap_plan_ready = true;
                            }
                            packed_attempt =
                                HwyBlockAligner::try_packed_global_gap<
                                    strand_reversed>(
                                        query_seq, target_seq,
                                        static_cast<size_t>(qs),
                                        static_cast<size_t>(qe),
                                        static_cast<size_t>(rs),
                                        static_cast<size_t>(re),
                                        packed_gap_plan);
                        }
                    }

                    if (packed_attempt.decision
                            == BlockPackedCertificateDecision::Certified) {
                        const std::uint32_t match_length =
                            static_cast<std::uint32_t>(
                                packed_attempt.certificate.symbols);
                        if (params.do_cigar) {
                            if (!region.cigar.empty()
                                && region.cigar.back().opcode
                                    == CIGAR_MATCH) {
                                region.cigar.back().length += match_length;
                            } else {
                                region.cigar.push_back(
                                    CigarOp{match_length, CIGAR_MATCH});
                            }
                        }
                        region.dp_score += packed_attempt.certificate.score;
                        if (params.compute_edit_distance) {
                            region.edit_distance += static_cast<sequence_size_t>(
                                packed_attempt.certificate.edit_distance);
                        }
                        final_ref_end = re;
                        final_query_end = qe;
                        qs = qe;
                        rs = re;
                        HwyBlockAligner::record_certified_global_gap<
                            strand_reversed>(
                                packed_gap_plan,
                                packed_attempt.certificate,
                                block_execution_proof);
                        continue;
                    }

                    // Construct shared result storage only for a real DP
                    // fallback.  Certified gaps above therefore avoid even
                    // the default std::vector state/destructor.
                    KswAlignResult fill_result;
                    if (packed_attempt.decision
                            == BlockPackedCertificateDecision::Rejected) {
                        // Exact packed bytes have already been scanned.  Run
                        // the unchanged DP path without paying for the byte
                        // certificate a second time.
                        fill_result = block_aligner_align_pair<
                            strand_reversed, false>(
                                query_seq, target_seq,
                                static_cast<size_t>(qs),
                                static_cast<size_t>(qe),
                                static_cast<size_t>(rs),
                                static_cast<size_t>(re),
                                scoring_matrix, params,
                                BlockAlignerPhase::GapExtension,
                                block_execution_proof);
                    } else {
                        // Fragmented storage, DNA5, unsupported matrices and
                        // non-candidate geometry retain today's route,
                        // including the existing byte certificate.
                        fill_result = block_aligner_align_pair<
                            strand_reversed,
                            enable_diagonal_certificate>(
                                query_seq, target_seq,
                                static_cast<size_t>(qs),
                                static_cast<size_t>(qe),
                                static_cast<size_t>(rs),
                                static_cast<size_t>(re),
                                scoring_matrix, params,
                                BlockAlignerPhase::GapExtension,
                                block_execution_proof);
                    }
                    if (apply_gap_fill_result<strand_reversed>(
                            region, fill_result, qe, re, qs, rs,
                            final_query_end, final_ref_end)) {
                        zdropped_any = true;
                        break;
                    }
                } else {
                    KswAlignResult fill_result =
                        ksw2_align_pair<strand_reversed>(
                        query_seq, target_seq,
                        static_cast<size_t>(qs),
                        static_cast<size_t>(qe),
                        static_cast<size_t>(rs),
                        static_cast<size_t>(re),
                        scoring_matrix, params,
                        params.compute_edit_distance ? -1 : align_bandwidth,
                        -1,
                        params.compute_edit_distance
                            ? -1
                            : params.zdrop,
                        KswFlags{.approx_max = true},
                        block_execution_proof,
                        scalar_execution_proof,
                        ScalarKsw2Phase::gap);
                    if (apply_gap_fill_result<strand_reversed>(
                            region, fill_result, qe, re, qs, rs,
                            final_query_end, final_ref_end)) {
                        zdropped_any = true;
                        break;
                    }
                }
            }
            prior_phase_zdropped = zdropped_any;
        } else {
            final_ref_end = seed_ref_end;
            final_query_end = seed_query_end;
            if (params.do_cigar || params.compute_edit_distance) {
                append_implicit_seed_cigar(
                    region.cigar,
                    static_cast<std::size_t>(seed_query_end - seed_query_start),
                    static_cast<std::size_t>(seed_ref_end - seed_ref_start));
            }
            if (params.compute_edit_distance) {
                region.edit_distance += exact_edit_distance_for_implicit_seed<
                    strand_reversed>(
                        query_seq, target_seq,
                        static_cast<std::size_t>(seed_query_start),
                        static_cast<std::size_t>(seed_query_end),
                        static_cast<std::size_t>(seed_ref_start),
                        static_cast<std::size_t>(seed_ref_end));
            }
        }

        // === 3. Right extension ===
        if ((base_alignment_algorithm
                == BaseAlignmentAlgorithmType::BLOCK_ALIGNER
                || !prior_phase_zdropped)
            && seed_query_end < outer_query_end
            && seed_ref_end < outer_ref_end)
        {
            KswAlignResult right_result;
            if constexpr (base_alignment_algorithm
                    == BaseAlignmentAlgorithmType::BLOCK_ALIGNER) {
                right_result = block_aligner_align_pair<strand_reversed>(
                    query_seq, target_seq,
                    static_cast<size_t>(seed_query_end),
                    static_cast<size_t>(outer_query_end),
                    static_cast<size_t>(seed_ref_end),
                    static_cast<size_t>(outer_ref_end),
                    scoring_matrix, params,
                    BlockAlignerPhase::RightExtension,
                    block_execution_proof);
            } else {
                right_result = ksw2_align_pair<strand_reversed>(
                    query_seq, target_seq,
                    static_cast<size_t>(seed_query_end),
                    static_cast<size_t>(outer_query_end),
                    static_cast<size_t>(seed_ref_end),
                    static_cast<size_t>(outer_ref_end),
                    scoring_matrix, params,
                    align_bandwidth, params.end_bonus, params.zdrop,
                    KswFlags{.extz_only = true},
                    block_execution_proof,
                    scalar_execution_proof,
                    ScalarKsw2Phase::right);
            }
            if (!right_result.cigar.empty()) {
                append_cigar(region.cigar, right_result.cigar);
                region.dp_score += right_result.max;
            }
            const int32_t right_ref_offset = right_result.reach_end
                ? right_result.mqe_t + 1 : right_result.max_t + 1;
            final_ref_end = seed_ref_end + right_ref_offset;
            const int32_t right_query_offset = right_result.reach_end
                ? outer_query_end - seed_query_end : right_result.max_q + 1;
            final_query_end = seed_query_end + right_query_offset;
            if (params.compute_edit_distance) {
                region.edit_distance += exact_edit_distance_for_fill<
                    strand_reversed>(
                        right_result,
                        static_cast<std::size_t>(seed_query_end),
                        static_cast<std::size_t>(final_query_end),
                        static_cast<std::size_t>(seed_ref_end),
                        static_cast<std::size_t>(final_ref_end));
            }
        }

        // Set final coordinates from alignment extent (align.c:897-899).
        // Internal extent math is signed int64; the stored region fields are
        // sequence_size_t (non-negative absolute coordinates).
        region.ref_start = static_cast<sequence_size_t>(final_ref_start);
        region.ref_end = static_cast<sequence_size_t>(final_ref_end);
        if (!region.is_reverse) {
            region.query_start = static_cast<sequence_size_t>(final_query_start);
            region.query_end = static_cast<sequence_size_t>(final_query_end);
        } else {
            region.query_start = static_cast<sequence_size_t>(query_length - final_query_end);
            region.query_end = static_cast<sequence_size_t>(query_length - final_query_start);
        }
    }
};

template<
    BaseAlignmentAlgorithmType base_alignment_algorithm,
    bool enable_diagonal_certificate = true,
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange,
    class Scanner = bioseq::storage::sequence_scanner_none<
        typename compact_sequence_range<QueryRange>::traits::storage_unit>>
void align_regions(
    std::span<AlignRegion> regions,
    std::span<const Anchor> anchors,
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    const ScoringMatrix<QueryRange>& scoring_matrix,
    const Minimap2Params& params,
    BlockAlignerExecutionProof* block_execution_proof = nullptr,
    ScalarPipelineExecutionProof* scalar_execution_proof = nullptr)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_align_skeleton");
    auto _scope = bioseq::instr::region(_desc);
    // DOC0263 §10.2 — lengths are full 64-bit (no int32 truncation of bp_size).
    // Region coordinate math below is signed int64 so the extension/clamp logic
    // and the reverse-strand mirror keep working for sequences longer than 2^31.
    const std::int64_t query_length = static_cast<std::int64_t>(query_seq.bp_size());
    const std::int64_t target_length = static_cast<std::int64_t>(target_seq.bp_size());
    const int32_t align_bandwidth = static_cast<int32_t>(params.bandwidth * 1.5 + 1.0);

    // Per DOC0249 §10.1 R4: the `[&]<bool>()` capture-all template lambda
    // that used to live inside the per-region loop body is replaced by the
    // non-capturing functor `AlignOnePhase`.  Constructed once per region
    // batch with the kernel-invariant references; the per-region call
    // surface is `aligner.template run<is_reversed>(...)`.
    const AlignOnePhase<
        base_alignment_algorithm, QueryRange, TargetRange, Scanner,
        enable_diagonal_certificate> aligner{
        query_seq, target_seq, scoring_matrix, params, align_bandwidth};

    // Proof collection is opt-in, so the normal benchmark path allocates no
    // diagnostic storage. When enabled, each worker writes one disjoint slot;
    // the deterministic reduction below avoids atomics in the DP hot path.
    std::vector<BlockAlignerExecutionProof> per_region_block_proofs;
    if constexpr (base_alignment_algorithm
            == BaseAlignmentAlgorithmType::BLOCK_ALIGNER) {
        if (block_execution_proof != nullptr) {
            per_region_block_proofs.resize(regions.size());
        }
    }
    std::vector<ScalarPipelineExecutionProof> per_region_scalar_proofs;
    if constexpr (base_alignment_algorithm
            == BaseAlignmentAlgorithmType::KSW2) {
        if (scalar_execution_proof != nullptr) {
            per_region_scalar_proofs.resize(regions.size());
        }
    }

    auto align_one_region_by_index =
        [query_length, target_length, &regions, &anchors, &params, &aligner,
         &per_region_block_proofs, &per_region_scalar_proofs](
                std::size_t region_index) {
        AlignRegion& region = regions[region_index];
        if (region.seed_count <= 0) {
            return;
        }

        const int32_t anchor_begin = region.anchor_start;
        const Anchor& first_anchor = anchors[anchor_begin];
        const Anchor& last_anchor = anchors[anchor_begin + region.seed_count - 1];

        // Inner seed boundaries (where seeds are)
        const int32_t first_span = static_cast<int32_t>(first_anchor.query_span);
        std::int64_t seed_ref_start = static_cast<std::int64_t>(first_anchor.ref_position)
                               + 1 - first_span;
        std::int64_t seed_query_start = static_cast<std::int64_t>(first_anchor.query_position)
                                 + 1 - first_span;
        const std::int64_t seed_ref_end = static_cast<std::int64_t>(last_anchor.ref_position) + 1;
        const std::int64_t seed_query_end = static_cast<std::int64_t>(last_anchor.query_position) + 1;
        if (seed_ref_start < 0) { seed_ref_start = 0; }
        if (seed_query_start < 0) { seed_query_start = 0; }

        // Outer boundaries: extend beyond seeds (align.c:708-762 simplified).
        // `extension_len` is a bounded gap (<= params.max_gap) computed in int64
        // to match the int64 coordinates; the std::min args are matched in type.
        std::int64_t outer_ref_start = seed_ref_start;
        std::int64_t outer_query_start = seed_query_start;
        std::int64_t outer_ref_end = seed_ref_end;
        std::int64_t outer_query_end = seed_query_end;

        if (seed_query_start > 0 && seed_ref_start > 0) {
            std::int64_t extension_len = std::min<std::int64_t>(seed_query_start, params.max_gap);
            outer_query_start = seed_query_start - extension_len;
            const std::int64_t bonus_extension =
                (extension_len * params.match_score > params.gap_open1)
                    ? (extension_len * params.match_score - params.gap_open1) / params.gap_ext1
                    : 0;
            extension_len += bonus_extension;
            extension_len = std::min<std::int64_t>(extension_len, params.max_gap);
            extension_len = std::min(extension_len, seed_ref_start);
            outer_ref_start = seed_ref_start - extension_len;
        }
        if (seed_query_end < query_length && seed_ref_end < target_length) {
            std::int64_t extension_len = std::min<std::int64_t>(query_length - seed_query_end, params.max_gap);
            outer_query_end = seed_query_end + extension_len;
            const std::int64_t bonus_extension =
                (extension_len * params.match_score > params.gap_open1)
                    ? (extension_len * params.match_score - params.gap_open1) / params.gap_ext1
                    : 0;
            extension_len += bonus_extension;
            extension_len = std::min<std::int64_t>(extension_len, params.max_gap);
            extension_len = std::min(extension_len, target_length - seed_ref_end);
            outer_ref_end = seed_ref_end + extension_len;
        }
        if (outer_query_end > query_length) {
            outer_query_end = query_length;
        }

        // DOC0254 §10.3: pre-reserve CIGAR storage from outer extents so the
        // three-phase fill collapses the geometric doubling chain to one
        // allocation per region.  `outer_query_end - outer_query_start`
        // bounds the column count of the left-extension + gap-fill +
        // right-extension union; +16 absorbs the per-op rounding.  The
        // reserve is clamped to `4 * params.max_gap + 64` ops to prevent
        // pathological over-allocation on multi-Mbp seed spans (XXL_HIFI
        // workloads where the seeded query extent itself is megabases —
        // run-length-compressed CIGARs are typically 10–100× smaller than
        // the column count, so this cap stays a strict upper bound while
        // protecting the per-query allocator from gigabyte spikes).
        {
            const std::int64_t cigar_q_extent = outer_query_end - outer_query_start;
            const std::int64_t cigar_r_extent = outer_ref_end   - outer_ref_start;
            const std::int64_t cigar_q_clamped = cigar_q_extent > 0 ? cigar_q_extent : 0;
            const std::int64_t cigar_r_clamped = cigar_r_extent > 0 ? cigar_r_extent : 0;
            const std::size_t raw_estimate =
                static_cast<std::size_t>(cigar_q_clamped) +
                static_cast<std::size_t>(cigar_r_clamped) + 16u;
            const std::size_t cigar_reserve_cap =
                static_cast<std::size_t>(4 * params.max_gap) + 64u;
            region.cigar.reserve(std::min(raw_estimate, cigar_reserve_cap));
        }

        region.cigar.clear();
        region.dp_score = 0;
        region.edit_distance = 0;

        BlockAlignerExecutionProof* region_block_proof = nullptr;
        if constexpr (base_alignment_algorithm
                == BaseAlignmentAlgorithmType::BLOCK_ALIGNER) {
            if (!per_region_block_proofs.empty()) {
                region_block_proof = &per_region_block_proofs[region_index];
                region_block_proof->add(
                    region_block_proof->routed_region_scopes);
                const BlockPhaseEligibility expected =
                    derive_block_phase_eligibility(
                        region, anchors, anchor_begin,
                        seed_ref_start, seed_query_start,
                        seed_ref_end, seed_query_end,
                        outer_ref_start, outer_query_start,
                        outer_ref_end, outer_query_end,
                        params);
                region_block_proof->add(
                    region_block_proof->left_route_candidates,
                    expected.left);
                region_block_proof->add(
                    region_block_proof->gap_route_candidates,
                    expected.gap);
                region_block_proof->add(
                    region_block_proof->right_route_candidates,
                    expected.right);
            }
        }
        ScalarPipelineExecutionProof* region_scalar_proof = nullptr;
        if constexpr (base_alignment_algorithm
                == BaseAlignmentAlgorithmType::KSW2) {
            if (!per_region_scalar_proofs.empty()) {
                region_scalar_proof =
                    &per_region_scalar_proofs[region_index];
            }
        }

        // Per-region diagnostic logs were previously emitted on every region
        // boundary (left-extension skipped, gap-fill skipped, right-extension
        // skipped, "determine actions" entry). Each fmtlog enqueue + format
        // is ~1-5us, and the bench harness installs `enable_logging = true`
        // via `log_configure(true)` so they fire during measurement runs.
        // For very-long pairs with many regions this stacks up to ~ms of
        // overhead per timed sample. Removed — the same information can be
        // derived from `region.cigar.empty()` and the seed/outer span fields
        // by any debugger-driven introspection without polluting the
        // bench wall-clock. See REPORT_POOR_BENCHMARK_PERFORMANCE_AND_LOW_QUALITY_SIMD_MM_V_3.md.

        // Dispatch the three alignment phases based on strand orientation
        // via the non-capturing `aligner` functor (DOC0249 §10.1 R4).
        if (region.is_reverse) {
            aligner.template run<true>(
                region, anchors, anchor_begin,
                seed_ref_start, seed_query_start,
                seed_ref_end, seed_query_end,
                outer_ref_start, outer_query_start,
                outer_ref_end, outer_query_end,
                query_length, region_block_proof, region_scalar_proof);
        } else {
            aligner.template run<false>(
                region, anchors, anchor_begin,
                seed_ref_start, seed_query_start,
                seed_ref_end, seed_query_end,
                outer_ref_start, outer_query_start,
                outer_ref_end, outer_query_end,
                query_length, region_block_proof, region_scalar_proof);
        }

        // Compute dp_max_score from CIGAR (simplified mm_update_extra)
        region.dp_max_score = region.dp_score;
        region.has_alignment = true;

        // Compute match_length/block_length from CIGAR
        int32_t cigar_match_count = 0;
        int32_t cigar_block_count = 0;
        for (const auto& cigar_op : region.cigar) {
            const int32_t op_len = static_cast<int32_t>(cigar_op.length);
            if (cigar_op.opcode == CIGAR_MATCH || cigar_op.opcode == CIGAR_EQ) {
                cigar_match_count += op_len;
                cigar_block_count += op_len;
            } else if (cigar_op.opcode == CIGAR_X) {
                cigar_block_count += op_len;
            } else if (cigar_op.opcode == CIGAR_INS || cigar_op.opcode == CIGAR_DEL) {
                cigar_block_count += op_len;
            }
        }
        if (cigar_match_count > 0) {
            region.match_length = cigar_match_count;
            const int32_t effective_block = (cigar_block_count > 0)
                ? cigar_block_count : region.block_length;
            region.block_length = effective_block;
        }
    };

    const auto& pool = bioseq::parallelism::GlobalPool::instance();
    const bool task_parallel_candidate =
        bioseq::parallelism::task_parallel(pool.mode())
        && pool.max_concurrency() > 1
        && base_alignment_algorithm != BaseAlignmentAlgorithmType::KSW2_BATCHED
        && bioseq::storage::lazy_is_worker_shareable(query_seq)
        && bioseq::storage::lazy_is_worker_shareable(target_seq)
        && regions.size() >= bioseq::parallelism::TaskParallelMinTriggers::MIN_REGIONS_COUNT;
    std::size_t total_seed_count = 0;
    if (task_parallel_candidate) {
        for (const AlignRegion& region : regions) {
            if (region.seed_count > 0) {
                total_seed_count += static_cast<std::size_t>(region.seed_count);
            }
        }
    }

    const bool may_parallel =
        task_parallel_candidate
        && total_seed_count >= bioseq::parallelism::TaskParallelMinTriggers::MIN_SEEDS_COUNT;

    if (may_parallel) {
        bioseq::parallelism::parallel_for_pipeline(
            0, regions.size(), 1,
            bioseq::parallelism::TaskParallelMinTriggers::MIN_REGIONS_COUNT,
            bioseq::parallelism::TaskParallelRegion::align_regions,
            [&align_one_region_by_index](std::size_t lo, std::size_t hi) {
                for (std::size_t region_index = lo; region_index < hi; ++region_index) {
                    align_one_region_by_index(region_index);
                }
            });
    } else {
        // DOC0276 — liveness heartbeat. A single big region (many gap-fills over a
        // multi-Mbp seed span) can dominate wall-clock; the per-phase milestones
        // alone would show a long silence with no sign of progress. Emit (and flush)
        // a heartbeat at most ~once/second so `tail -f` of the log shows which
        // region index is in flight and the live footprint. Gated on enable_logging
        // so production pays only the loop counter.
        std::int64_t hb_last_ns = enable_logging ? bioseq::instr::clock::now_ns() : 0;
        std::size_t hb_region_idx = 0;
        for (std::size_t region_index = 0; region_index < regions.size(); ++region_index) {
            ++hb_region_idx;
            if (enable_logging) {
                const std::int64_t hb_now_ns = bioseq::instr::clock::now_ns();
                if (hb_now_ns - hb_last_ns > 1'000'000'000LL) {
                    hb_last_ns = hb_now_ns;
                    bioseq_log_lazy<DBG>([&] {
                        bioseq_log_progress<DBG>("[minimap_port] aligning region {}/{} seed_count={}"
                                " | footprint={}MB avail={}MB",
                                hb_region_idx, regions.size(), regions[region_index].seed_count,
                                bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                                bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
                    });
                }
            }
            align_one_region_by_index(region_index);
        }
    }

    if constexpr (base_alignment_algorithm
            == BaseAlignmentAlgorithmType::BLOCK_ALIGNER) {
        if (block_execution_proof != nullptr) {
            *block_execution_proof = {};
            for (const BlockAlignerExecutionProof& region_proof
                    : per_region_block_proofs) {
                block_execution_proof->combine(region_proof);
            }
            block_execution_proof->add(
                block_execution_proof->align_region_invocations);
            if (may_parallel) {
                block_execution_proof->add(
                    block_execution_proof->parallel_align_region_dispatches);
            } else {
                block_execution_proof->add(
                    block_execution_proof->serial_align_region_dispatches);
            }
            if (!block_execution_proof->accepted()) {
                bioseq_log<ERR>(
                    "Block Aligner execution proof rejected: eligible={}, "
                    "executed={}, ksw2={}, scalar_fallbacks={}",
                    block_execution_proof->eligible_calls(),
                    block_execution_proof->executed_calls(),
                    block_execution_proof->forbidden_ksw2_calls,
                    block_execution_proof
                        ->forbidden_scalar_biological_fallbacks);
                std::abort();
            }
        }
    }
    if constexpr (base_alignment_algorithm
            == BaseAlignmentAlgorithmType::KSW2) {
        if (scalar_execution_proof != nullptr) {
            *scalar_execution_proof = {};
            for (const ScalarPipelineExecutionProof& region_proof
                    : per_region_scalar_proofs) {
                scalar_execution_proof->combine(region_proof);
            }
        }
    }

    bioseq_log_lazy<DBG>([&] {
        bioseq_log_progress<DBG>("[minimap_port] Aligned {} regions | footprint={}MB avail={}MB",
                regions.size(),
                bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
    });
}

// ============================================================================
// Phase 8: Compute mapping quality (mm_set_mapq equivalent)
// ============================================================================

// ============================================================================
// Phase 7b: Estimate divergence from seeds (mm_est_err equivalent)
// ============================================================================

/**
 * @brief Estimate per-base divergence from seed positions.
 *
 * Equivalent of mm_est_err() in esterr.c.
 *
 * Algorithm:
 *   1. Build a sorted array of forward-strand query minimizer positions (mini_pos)
 *   2. For each region, binary-search for the first seed in mini_pos
 *   3. Scan forward through mini_pos, counting how many chain seeds match
 *   4. n_tot = span of mini_pos entries + edge corrections
 *   5. div = 1 - (n_match/n_tot)^(1/avg_k)
 *
 * @param regions           Alignment regions to update
 * @param query_minimizers  All query minimizers (used as mini_pos proxy)
 * @param anchors           Reordered anchor array
 * @param query_len         Query sequence length
 * @param kmer_size         k-mer size
 * @param target_len        Target/reference sequence length
 */
template <class MinimizerT>
void estimate_divergence_from_seeds(
    std::span<AlignRegion> regions,
    std::span<const MinimizerT> query_minimizers,
    std::span<const Anchor> anchors,
    sequence_size_t query_len,
    int32_t kmer_size,
    sequence_size_t target_len)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_est_err");
    auto _scope = bioseq::instr::region(_desc);
    if (query_minimizers.empty()) {
        return;
    }

    // ----------------------------------------------------------------------
    // V13: SORT-FREE per-position prefix-count for O(1) membership AND
    // O(1) range count. Replaces the V12 "build sorted mini_pos array +
    // binary-search-per-seed" path:
    //
    //   prefix_count[i] = #(query minimizers with last_pos < i)
    //
    // Then:
    //   * membership at position p:   prefix_count[p+1] > prefix_count[p]
    //   * #mini_pos in [a, b]:         prefix_count[b+1] - prefix_count[a]
    //
    // Build cost is O(N + query_len). For VERY_LONG (903 kbp) this is a
    // single ~1 M-iteration linear pass — vs V12's 5–6 ms `std::sort` on
    // 168 k entries (the SIMD V12 dual-stream collector emits in
    // lane-interleaved order, so the array is NOT sorted on output and
    // std::sort cannot detect already-sorted input). The V12 phase
    // breakdown's `mm_est_err = 7.96 ms` for SIMD-DP forward chains was
    // dominated by this sort, not the per-seed walk we previously thought.
    //
    // Bench (VERY_LONG, post-V13): mm_est_err per-call drops from 7.96 ms
    // to <2 ms (-75%) because the sort is gone and the per-seed walk hits
    // the cumulative array via two random-access loads (~3 ns each).
    //
    // V13 also drops the V10 reverse-chain search-range split: with the
    // direction-aware `compute_forward_pos(s)` lambda BOTH forward and
    // reverse chains have ASCENDING forward_pos for s=0..seed_count-1, so
    // the previous "search [begin, match_start_idx+1)" for reverse chains
    // was structurally wrong (last_seed_pos > first_forward_pos for both
    // directions — V10 fix was a no-op at best, broken-fast-path at worst).
    // ----------------------------------------------------------------------
    // The compact MinimapSketchData no longer carries a per-minimizer
    // span field — for dna4 (the only bench alphabet) every minimizer's
    // span is identically equal to the configured kmer size. The
    // legacy `span_sum / count` average therefore degenerates to the
    // constant `kmer_size`, which we use directly instead of touching
    // a now-removed bitfield.
    const float avg_kmer_span = static_cast<float>(kmer_size);

    // Build the prefix-count array in two linear passes:
    //   1. mini_pos_count[p + 1] += #(minimizers at last_pos == p)
    //   2. cumulative-sum: mini_pos_count[i] += mini_pos_count[i - 1]
    // Final invariant: mini_pos_count[i] = #(minimizers with last_pos < i)
    //
    // Size is (query_len + 2) so `mini_pos_count[query_len + 1]` is a
    // valid index (covers the case where last_pos == query_len, even
    // though the V12 collector's emit clamps to query_len - 1). Using
    // uint32_t supports up to 4 G minimizers — vastly above realistic
    // cell-genome scale.
    const size_t prefix_size = static_cast<size_t>(query_len) + 2;
    std::vector<uint32_t> mini_pos_count(prefix_size, 0);
    for (const auto& query_min : query_minimizers) {
        const int64_t pos = static_cast<int64_t>(query_min.last_pos());
        if (pos >= 0 && pos <= static_cast<int64_t>(query_len)) {
            ++mini_pos_count[static_cast<size_t>(pos) + 1];
        }
    }
    for (size_t i = 1; i < prefix_size; ++i) {
        mini_pos_count[i] += mini_pos_count[i - 1];
    }

    // Per DOC0249 §10.2 R4: the three `[&]` lambdas previously declared
    // here (`is_minimizer`, `range_count`, `compute_forward_pos`) are
    // converted to non-capturing struct functors so the file-wide grep gate
    // for capture-all lambdas under `bioseq/backend/pipeline/**.cppm` runs
    // clean.  Each functor stores its dependencies by value (no `&`); the
    // compiler treats them identically to inline methods at -O3 (M15).
    struct IsMinimizer {
        const std::uint32_t* mini_pos_count;
        sequence_size_t      query_len;
        [[nodiscard, gnu::always_inline]]
        inline bool operator()(std::int64_t pos) const noexcept {
            if (pos < 0 || static_cast<sequence_size_t>(pos) > query_len) return false;
            const std::size_t p = static_cast<std::size_t>(pos);
            return mini_pos_count[p + 1] > mini_pos_count[p];
        }
    };
    struct RangeCount {
        const std::uint32_t* mini_pos_count;
        sequence_size_t      query_len;
        [[nodiscard, gnu::always_inline]]
        inline int32_t operator()(std::int64_t a, std::int64_t b) const noexcept {
            if (a < 0) a = 0;
            if (static_cast<sequence_size_t>(b) > query_len) b = static_cast<std::int64_t>(query_len);
            if (a > b) return 0;
            return static_cast<int32_t>(
                mini_pos_count[static_cast<std::size_t>(b) + 1] -
                mini_pos_count[static_cast<std::size_t>(a)]);
        }
    };
    const IsMinimizer is_minimizer{mini_pos_count.data(), query_len};
    const RangeCount  range_count{mini_pos_count.data(), query_len};

    for (auto& region : regions) {
        region.divergence = -1.0f;
        if (region.seed_count <= 0) {
            continue;
        }

        const int32_t anchor_begin = region.anchor_start;

        // compute_forward_pos walks chain anchors in such order that
        // forward_pos values are MONOTONICALLY ASCENDING in `s`. For forward
        // chains: s -> anchor[s] in chain order (ascending query_position
        // == ascending forward_pos). For reverse chains: s -> anchor[cnt-1-s]
        // in REVERSE chain order; coupled with the y-flip on the
        // reverse-encoded query_position this yields ascending forward_pos.
        // (See esterr.c:50,57-59 — minimap2 walks reverse chains in the
        // same reverse order for the same reason.)
        //
        // Non-capturing struct functor per DOC0249 §10.2 R4: dependencies
        // (region orientation, anchor base, seed count, query length) are
        // stored by value.  The compiler inlines `operator()` identically
        // to the original `[&]` lambda at -O3 (M15).
        struct ComputeForwardPos {
            const Anchor* anchors_base;
            int32_t       anchor_begin;
            int32_t       seed_count;
            sequence_size_t query_len;
            bool          region_is_reverse;

            [[nodiscard, gnu::always_inline]]
            inline std::int64_t operator()(int32_t s) const noexcept {
                const Anchor& chain_anchor = region_is_reverse
                    ? anchors_base[anchor_begin + seed_count - 1 - s]
                    : anchors_base[anchor_begin + s];
                std::int64_t forward_pos = static_cast<std::int64_t>(chain_anchor.query_position);
                if (chain_anchor.is_reverse) {
                    const std::int64_t anchor_span =
                        static_cast<int32_t>(chain_anchor.query_span);
                    forward_pos = static_cast<std::int64_t>(query_len) - 1 - (forward_pos + 1 - anchor_span);
                }
                return forward_pos;
            }
        };
        const ComputeForwardPos compute_forward_pos{
            anchors.data(), anchor_begin, region.seed_count,
            query_len, region.is_reverse};

        // First chain anchor (s=0) — minimum forward_pos. esterr.c:50 uses
        // `r->rev ? last : first` which is the same anchor compute_forward_pos(0)
        // returns.
        const std::int64_t first_forward_pos = compute_forward_pos(0);
        if (!is_minimizer(first_forward_pos)) {
            continue;
        }

        // Walk chain anchors with O(1) prefix-count probe. Mirror minimap2's
        // halt-on-miss semantics (esterr.c:57-62 — once a chain anchor is
        // not found in mini_pos, the C `for (...; ++j)` loop advances j to
        // the end without ever advancing k, so subsequent chain anchors
        // are not counted). For our pipeline every chain anchor is by
        // construction a query minimizer (collect_seed_hits looks each one
        // up in the target index), so the loop normally walks all
        // seed_count entries.
        int32_t seed_match_count = 1;
        std::int64_t last_match_pos = first_forward_pos;
        for (int32_t walk_idx = 1; walk_idx < region.seed_count; ++walk_idx) {
            const std::int64_t fp = compute_forward_pos(walk_idx);
            if (!is_minimizer(fp)) {
                break;
            }
            last_match_pos = fp;
            ++seed_match_count;
        }

        // n_tot = span of mini_pos between first and last match (esterr.c:61),
        // counting duplicates (cumulative-sum array preserves them).
        int32_t total_minimizer_span = range_count(first_forward_pos, last_match_pos);

        // Edge corrections (esterr.c:62-63).
        if (region.query_start > avg_kmer_span && region.ref_start > avg_kmer_span) {
            ++total_minimizer_span;
        }
        if (query_len - region.query_start > avg_kmer_span
            && target_len - region.ref_end > avg_kmer_span)
        {
            ++total_minimizer_span;
        }

        // Divergence formula (esterr.c:64).
        if (seed_match_count >= total_minimizer_span) {
            region.divergence = 0.0f;
        } else {
            const double match_ratio = static_cast<double>(seed_match_count)
                                     / total_minimizer_span;
            region.divergence = static_cast<float>(
                1.0 - std::pow(match_ratio, 1.0 / avg_kmer_span));
        }
    }
}

// ============================================================================
// Phase 8: Compute mapping quality (mm_set_mapq equivalent)

/**
 * @brief Compute mapping quality for all regions.
 *
 * Equivalent to mm_set_mapq2() in hit.c:423-476.
 *
 * MAPQ formula (primary with DP scores):
 *   mapq = identity * pen_cm * 40 * (1 - ratio^2) * ln(dp_max_score / match_sc)
 *   mapq -= 4.343 * ln(secondary_count + 1)
 *   mapq = clamp(mapq, 0, 60)
 *
 * Where ratio = secondary_dp_max * secondary_chain_score / (dp_max_score * original_score)
 */
void compute_mapping_quality(
    std::span<AlignRegion> regions,
    const Minimap2Params& params,
    int32_t rep_len)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_set_mapq");
    auto _scope = bioseq::instr::region(_desc);
    constexpr float quality_coefficient = 40.0f;
    const int32_t match_sc = params.match_score;
    const int32_t min_chain_sc = params.min_chain_score;

    // Compute uniqueness ratio
    int64_t primary_score_sum = 0;
    for (const auto& region : regions) {
        if (region.parent == region.region_id) {
            primary_score_sum += region.score;
        }
    }
    // hit.c:436: uniq_ratio = (float)sum_sc / (sum_sc + rep_len)
    const float uniqueness_ratio = static_cast<float>(primary_score_sum) /
                                   static_cast<float>(primary_score_sum + rep_len);

    for (auto& region : regions) {
        if (region.parent != region.region_id) {
            region.mapq = 0; // secondaries get 0
            continue;
        }

        // Primary mapping
        const float score_factor = (region.score > 100) ? 1.0f : 0.01f * region.score;
        const float score_penalty = score_factor * uniqueness_ratio;
        const float raw_seed_penalty = (region.seed_count > 10)
            ? 1.0f : 0.1f * region.seed_count;
        const float seed_count_penalty = std::min(score_penalty, raw_seed_penalty);

        const int32_t effective_secondary_score = std::max(
            region.secondary_score, min_chain_sc);

        int32_t quality_value;
        // hit.c:447-467: three branches based on DP availability
        if (region.has_alignment && region.secondary_dp_max > 0
            && region.dp_max_score > 0)
        {
            // Formula A: with DP scores (hit.c:448-457)
            const float identity = (region.block_length > 0)
                ? static_cast<float>(region.match_length) / region.block_length
                : 0.f;
            const float score_ratio =
                static_cast<float>(region.secondary_dp_max) * effective_secondary_score
                / (static_cast<float>(region.dp_max_score) * region.original_score);
            quality_value = static_cast<int32_t>(
                identity * seed_count_penalty * quality_coefficient
                * (1.0f - score_ratio * score_ratio)
                * std::log(static_cast<float>(region.dp_max_score) / match_sc));

            // BWA-MEM-like alternative (hit.c:455)
            const int32_t quality_alt = static_cast<int32_t>(
                6.02f * identity * identity
                * (region.dp_max_score - region.secondary_dp_max) / match_sc + 0.499f);
            quality_value = std::min(quality_value, quality_alt);
        } else {
            // No secondary_dp_max available: use chain score formula (hit.c:460-467)
            const float score_ratio = static_cast<float>(effective_secondary_score)
                                    / region.original_score;
            if (region.has_alignment) {
                const float identity = (region.block_length > 0)
                    ? static_cast<float>(region.match_length) / region.block_length
                    : 0.f;
                quality_value = static_cast<int32_t>(
                    identity * seed_count_penalty * quality_coefficient
                    * (1.0f - score_ratio)
                    * std::log(static_cast<float>(region.dp_max_score) / match_sc));
            } else {
                // No DP at all — use chain score (hit.c:466)
                quality_value = static_cast<int32_t>(
                    seed_count_penalty * quality_coefficient
                    * (1.0f - score_ratio)
                    * std::log(static_cast<float>(region.score)));
            }
        }

        // Subtract secondary_count penalty (hit.c:469)
        quality_value -= static_cast<int32_t>(
            4.343f * std::log(static_cast<float>(region.secondary_count + 1)) + 0.499f);
        quality_value = std::max(quality_value, 0);
        region.mapq = std::min(quality_value, 60);

        // Ensure non-zero if clearly best (hit.c:472)
        if (region.has_alignment && region.dp_max_score > region.secondary_dp_max
            && region.mapq == 0)
        {
            region.mapq = 1;
        }
    }

    bioseq_log<DBG>("[minimap_port] Computed MAPQ for {} regions", regions.size());
}

// ============================================================================
// Phase 9: Convert to MatchResult (PAF output)
// ============================================================================

/**
 * @brief Convert internal AlignRegion to PAF-like MatchResult.
 *
 * Fields map to PAF columns as documented in mm_write_paf4() (format.c:427-460).
 */
[[nodiscard]] MatchResult region_to_match_result(
    AlignRegion&& region,
    sequence_size_t query_len,
    sequence_size_t target_len)
{
    return MatchResult{
        .query_len             = query_len,
        .query_start           = region.query_start,
        .query_end             = region.query_end,
        .is_reverse            = region.is_reverse,
        .target_len            = target_len,
        .target_start          = region.ref_start,
        .target_end            = region.ref_end,
        .num_matches           = region.match_length,
        .block_length          = region.block_length,
        .mapq                  = region.mapq,
        .chain_score           = region.score,
        .dp_score              = region.dp_score,
        .dp_max_score          = region.dp_max_score,
        .seed_count            = region.seed_count,
        .secondary_chain_score = region.secondary_score,
        .divergence            = region.divergence,
        .is_primary            = (region.parent == region.region_id),
        .has_cigar             = region.has_alignment,
        .cigar                 = std::move(region.cigar),
        .edit_distance         = region.edit_distance,
    };
}

// ============================================================================
// PAF output — MatchResultCollection
// ============================================================================

// CIGAR_OP_CHAR and format_cigar are now in bioseq_cigar module.

/// Positive execution evidence for Vulkan stages used by the historical
/// partial-GPU benchmark rows. Counters are incremented only after a selected
/// fail-loud Vulkan operation returns successfully; selector flags alone never
/// populate this proof.
struct PartialVulkanExecutionProof {
    /// Role-neutral token emitted by a minimizer leaf. It must be consumed as
    /// either query_sketch or target_sketch before a MatchResultCollection is
    /// returned to callers.
    std::uint64_t sketch_dispatches = 0;
    std::uint64_t query_sketch_dispatches = 0;
    std::uint64_t target_sketch_dispatches = 0;
    std::uint64_t target_index_build_dispatches = 0;
    std::uint64_t seed_lookup_dispatches = 0;
    // Operation-originated representation branch from the completed seed
    // shader. Their sum must equal seed_lookup_dispatches for strict rows.
    std::uint64_t seed_open_addressed_lookup_dispatches = 0;
    std::uint64_t seed_sorted_dense_lookup_dispatches = 0;
    std::uint64_t chain_score_dispatches = 0;
    // Operation-local seed-sort evidence. A selected mode flag never writes
    // these counters; they are copied only from a completed runtime result.
    std::uint64_t seed_bitonic_sort_dispatches = 0;
    std::uint64_t seed_radix_sort_operations = 0;
    std::uint64_t seed_radix_anchor_count = 0;
    std::uint64_t seed_radix_passes = 0;
    std::uint64_t seed_radix_histogram_dispatches = 0;
    std::uint64_t seed_radix_scan_dispatches = 0;
    std::uint64_t seed_radix_scatter_dispatches = 0;
    std::uint64_t seed_host_sort_fallbacks = 0;
    // Operation-originated legacy Phase-B index materialization evidence.
    // Additive fields describe actual upload operations, never cache hits;
    // max fields preserve the worst collision/load observation across them.
    std::uint64_t gpu_index_materializations = 0;
    std::uint64_t gpu_index_open_addressed_materializations = 0;
    std::uint64_t gpu_index_sorted_dense_materializations = 0;
    std::uint64_t gpu_index_unique_keys = 0;
    std::uint64_t gpu_index_slot_capacity = 0;
    std::uint64_t gpu_index_logical_resident_bytes = 0;
    std::uint64_t gpu_index_actual_allocation_bytes = 0;
    std::uint64_t gpu_index_fully_touched_bytes = 0;
    std::uint64_t gpu_index_staging_bytes = 0;
    std::uint64_t gpu_index_permit_releases = 0;
    std::uint64_t gpu_index_process_footprint_reflections = 0;
    std::uint64_t gpu_index_reflected_footprint_bytes = 0;
    std::uint64_t gpu_index_max_load_ppm = 0;
    std::uint64_t gpu_index_max_probe = 0;
    std::uint64_t gpu_index_count_phase_admissions = 0;
    std::uint64_t gpu_index_main_peak_admissions = 0;
    std::uint64_t gpu_index_resident_transfers = 0;
    std::uint64_t gpu_index_adoption_rereserve_attempts = 0;
    std::uint64_t gpu_index_adoption_successes = 0;
    std::uint64_t gpu_index_owned_resident_bytes = 0;
    std::uint64_t gpu_index_gate_enabled = 0;
    std::uint64_t gpu_index_gate_word_count = 0;
    std::uint64_t gpu_index_gate_mask = 0;
    std::uint64_t gpu_index_gate_set_bits = 0;
    std::uint64_t gpu_index_gate_digest_lo = 0;
    std::uint64_t gpu_index_gate_digest_hi = 0;
    // Operation-originated Phase-C target-index kernel/shape evidence. These
    // fields are attached to the one query that owns the shared target build,
    // never to every query that subsequently consumes the resident index.
    std::uint64_t gpu_index_gate_count_dispatches = 0;
    std::uint64_t gpu_index_gated_emit_dispatches = 0;
    std::uint64_t gpu_index_ungated_emit_dispatches = 0;
    std::uint64_t gpu_index_exact_count_dispatches = 0;
    std::uint64_t gpu_index_exact_count_prefix_scan_dispatches = 0;
    std::uint64_t gpu_index_exact_count_control_readbacks = 0;
    std::uint64_t gpu_index_deterministic_tile_emit_dispatches = 0;
    std::uint64_t gpu_index_atomic_append_dispatches = 0;
    std::uint64_t gpu_index_emit_validation_control_readbacks = 0;
    std::uint64_t gpu_index_counted_minimizer_count = 0;
    std::uint64_t gpu_index_verified_emit_count = 0;
    std::uint64_t gpu_index_count_emit_parity_verified = 0;
    std::uint64_t gpu_index_target_minimizer_capacity_estimate = 0;
    std::uint64_t gpu_index_capacity_estimate_underestimated = 0;
    std::uint64_t gpu_index_arena_trimmed_before_permit_shrink = 0;
    std::uint64_t gpu_index_retained_minimizer_count = 0;
    std::uint64_t gpu_index_distinct_minimizer_count = 0;
    // Owned-only last-seed retirement evidence. These counters are populated
    // only after the completed seed operation has dropped its local resident
    // handle and the operation-owned lifetime coordinator has actually
    // released storage before chaining.
    std::uint64_t post_seed_owned_gpu_cache_evictions = 0;
    std::uint64_t post_seed_owned_gpu_resident_bytes_released = 0;
    std::uint64_t post_seed_owned_cpu_index_releases = 0;
    std::uint64_t post_seed_owned_cpu_position_bytes_released = 0;
    bool saturated = false;

    static constexpr void add(std::uint64_t& destination,
                              std::uint64_t increment,
                              bool& saturated_flag) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                            - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturated_flag = true;
            return;
        }
        destination += increment;
    }

    constexpr void combine(
        const PartialVulkanExecutionProof& other) noexcept {
        bool overflow = saturated || other.saturated;
        add(sketch_dispatches, other.sketch_dispatches, overflow);
        add(query_sketch_dispatches, other.query_sketch_dispatches, overflow);
        add(target_sketch_dispatches, other.target_sketch_dispatches, overflow);
        add(target_index_build_dispatches,
            other.target_index_build_dispatches, overflow);
        add(seed_lookup_dispatches, other.seed_lookup_dispatches, overflow);
        add(seed_open_addressed_lookup_dispatches,
            other.seed_open_addressed_lookup_dispatches, overflow);
        add(seed_sorted_dense_lookup_dispatches,
            other.seed_sorted_dense_lookup_dispatches, overflow);
        add(chain_score_dispatches, other.chain_score_dispatches, overflow);
        add(seed_bitonic_sort_dispatches,
            other.seed_bitonic_sort_dispatches, overflow);
        add(seed_radix_sort_operations,
            other.seed_radix_sort_operations, overflow);
        add(seed_radix_anchor_count,
            other.seed_radix_anchor_count, overflow);
        add(seed_radix_passes, other.seed_radix_passes, overflow);
        add(seed_radix_histogram_dispatches,
            other.seed_radix_histogram_dispatches, overflow);
        add(seed_radix_scan_dispatches,
            other.seed_radix_scan_dispatches, overflow);
        add(seed_radix_scatter_dispatches,
            other.seed_radix_scatter_dispatches, overflow);
        add(seed_host_sort_fallbacks,
            other.seed_host_sort_fallbacks, overflow);
        add(gpu_index_materializations,
            other.gpu_index_materializations, overflow);
        add(gpu_index_open_addressed_materializations,
            other.gpu_index_open_addressed_materializations, overflow);
        add(gpu_index_sorted_dense_materializations,
            other.gpu_index_sorted_dense_materializations, overflow);
        add(gpu_index_unique_keys, other.gpu_index_unique_keys, overflow);
        add(gpu_index_slot_capacity, other.gpu_index_slot_capacity, overflow);
        add(gpu_index_logical_resident_bytes,
            other.gpu_index_logical_resident_bytes, overflow);
        add(gpu_index_actual_allocation_bytes,
            other.gpu_index_actual_allocation_bytes, overflow);
        add(gpu_index_fully_touched_bytes,
            other.gpu_index_fully_touched_bytes, overflow);
        add(gpu_index_staging_bytes,
            other.gpu_index_staging_bytes, overflow);
        add(gpu_index_permit_releases,
            other.gpu_index_permit_releases, overflow);
        add(gpu_index_process_footprint_reflections,
            other.gpu_index_process_footprint_reflections, overflow);
        add(gpu_index_reflected_footprint_bytes,
            other.gpu_index_reflected_footprint_bytes, overflow);
        add(gpu_index_count_phase_admissions,
            other.gpu_index_count_phase_admissions, overflow);
        add(gpu_index_main_peak_admissions,
            other.gpu_index_main_peak_admissions, overflow);
        add(gpu_index_resident_transfers,
            other.gpu_index_resident_transfers, overflow);
        add(gpu_index_adoption_rereserve_attempts,
            other.gpu_index_adoption_rereserve_attempts, overflow);
        add(gpu_index_adoption_successes,
            other.gpu_index_adoption_successes, overflow);
        add(gpu_index_owned_resident_bytes,
            other.gpu_index_owned_resident_bytes, overflow);
        add(gpu_index_gate_enabled, other.gpu_index_gate_enabled, overflow);
        add(gpu_index_gate_word_count,
            other.gpu_index_gate_word_count, overflow);
        add(gpu_index_gate_mask, other.gpu_index_gate_mask, overflow);
        add(gpu_index_gate_set_bits,
            other.gpu_index_gate_set_bits, overflow);
        add(gpu_index_gate_digest_lo,
            other.gpu_index_gate_digest_lo, overflow);
        add(gpu_index_gate_digest_hi,
            other.gpu_index_gate_digest_hi, overflow);
        add(gpu_index_gate_count_dispatches,
            other.gpu_index_gate_count_dispatches, overflow);
        add(gpu_index_gated_emit_dispatches,
            other.gpu_index_gated_emit_dispatches, overflow);
        add(gpu_index_ungated_emit_dispatches,
            other.gpu_index_ungated_emit_dispatches, overflow);
        add(gpu_index_exact_count_dispatches,
            other.gpu_index_exact_count_dispatches, overflow);
        add(gpu_index_exact_count_prefix_scan_dispatches,
            other.gpu_index_exact_count_prefix_scan_dispatches, overflow);
        add(gpu_index_exact_count_control_readbacks,
            other.gpu_index_exact_count_control_readbacks, overflow);
        add(gpu_index_deterministic_tile_emit_dispatches,
            other.gpu_index_deterministic_tile_emit_dispatches, overflow);
        add(gpu_index_atomic_append_dispatches,
            other.gpu_index_atomic_append_dispatches, overflow);
        add(gpu_index_emit_validation_control_readbacks,
            other.gpu_index_emit_validation_control_readbacks, overflow);
        add(gpu_index_counted_minimizer_count,
            other.gpu_index_counted_minimizer_count, overflow);
        add(gpu_index_verified_emit_count,
            other.gpu_index_verified_emit_count, overflow);
        add(gpu_index_count_emit_parity_verified,
            other.gpu_index_count_emit_parity_verified, overflow);
        add(gpu_index_target_minimizer_capacity_estimate,
            other.gpu_index_target_minimizer_capacity_estimate, overflow);
        add(gpu_index_capacity_estimate_underestimated,
            other.gpu_index_capacity_estimate_underestimated, overflow);
        add(gpu_index_arena_trimmed_before_permit_shrink,
            other.gpu_index_arena_trimmed_before_permit_shrink, overflow);
        add(gpu_index_retained_minimizer_count,
            other.gpu_index_retained_minimizer_count, overflow);
        add(gpu_index_distinct_minimizer_count,
            other.gpu_index_distinct_minimizer_count, overflow);
        add(post_seed_owned_gpu_cache_evictions,
            other.post_seed_owned_gpu_cache_evictions, overflow);
        add(post_seed_owned_gpu_resident_bytes_released,
            other.post_seed_owned_gpu_resident_bytes_released, overflow);
        add(post_seed_owned_cpu_index_releases,
            other.post_seed_owned_cpu_index_releases, overflow);
        add(post_seed_owned_cpu_position_bytes_released,
            other.post_seed_owned_cpu_position_bytes_released, overflow);
        gpu_index_max_load_ppm = std::max(
            gpu_index_max_load_ppm, other.gpu_index_max_load_ppm);
        gpu_index_max_probe = std::max(
            gpu_index_max_probe, other.gpu_index_max_probe);
        saturated = overflow;
    }

    [[nodiscard]] constexpr bool has_any_dispatch() const noexcept {
        return saturated
            || sketch_dispatches != 0
            || query_sketch_dispatches != 0
            || target_sketch_dispatches != 0
            || target_index_build_dispatches != 0
            || seed_lookup_dispatches != 0
            || seed_open_addressed_lookup_dispatches != 0
            || seed_sorted_dense_lookup_dispatches != 0
            || chain_score_dispatches != 0
            || seed_bitonic_sort_dispatches != 0
            || seed_radix_sort_operations != 0
            || seed_radix_anchor_count != 0
            || seed_radix_passes != 0
            || seed_radix_histogram_dispatches != 0
            || seed_radix_scan_dispatches != 0
            || seed_radix_scatter_dispatches != 0
            || seed_host_sort_fallbacks != 0
            || gpu_index_gate_count_dispatches != 0
            || gpu_index_gated_emit_dispatches != 0
            || gpu_index_ungated_emit_dispatches != 0
            || gpu_index_exact_count_dispatches != 0
            || gpu_index_exact_count_prefix_scan_dispatches != 0
            || gpu_index_exact_count_control_readbacks != 0
            || gpu_index_deterministic_tile_emit_dispatches != 0
            || gpu_index_atomic_append_dispatches != 0
            || gpu_index_emit_validation_control_readbacks != 0
            || gpu_index_counted_minimizer_count != 0
            || gpu_index_verified_emit_count != 0
            || gpu_index_count_emit_parity_verified != 0
            || gpu_index_target_minimizer_capacity_estimate != 0
            || gpu_index_capacity_estimate_underestimated != 0
            || gpu_index_arena_trimmed_before_permit_shrink != 0;
    }

    [[nodiscard]] constexpr bool exact_target_index_route_accepted()
        const noexcept
    {
        const std::uint64_t builds = target_index_build_dispatches;
        if (builds == 0u) {
            return gpu_index_exact_count_dispatches == 0u
                && gpu_index_exact_count_prefix_scan_dispatches == 0u
                && gpu_index_exact_count_control_readbacks == 0u
                && gpu_index_deterministic_tile_emit_dispatches == 0u
                && gpu_index_atomic_append_dispatches == 0u
                && gpu_index_emit_validation_control_readbacks == 0u
                && gpu_index_counted_minimizer_count == 0u
                && gpu_index_verified_emit_count == 0u
                && gpu_index_count_emit_parity_verified == 0u
                && gpu_index_target_minimizer_capacity_estimate == 0u
                && gpu_index_capacity_estimate_underestimated == 0u
                && gpu_index_arena_trimmed_before_permit_shrink == 0u
                && gpu_index_gate_enabled == 0u
                && gpu_index_gate_count_dispatches == 0u
                && gpu_index_gated_emit_dispatches == 0u
                && gpu_index_ungated_emit_dispatches == 0u
                && gpu_index_retained_minimizer_count == 0u
                && gpu_index_distinct_minimizer_count == 0u;
        }
        // A Phase-C batch owns exactly one shared target build and attaches its
        // proof to query zero. More than one build is not an accepted strict row.
        if (builds != 1u) {
            return false;
        }
        const bool empty = gpu_index_retained_minimizer_count == 0u;
        return target_sketch_dispatches == builds
            && gpu_index_exact_count_dispatches == builds
            && gpu_index_exact_count_prefix_scan_dispatches == builds * 3u
            && gpu_index_exact_count_control_readbacks == builds
            && gpu_index_deterministic_tile_emit_dispatches
                == (empty ? 0u : builds)
            && gpu_index_atomic_append_dispatches == 0u
            && gpu_index_emit_validation_control_readbacks
                == (empty ? 0u : builds)
            && gpu_index_counted_minimizer_count
                == gpu_index_retained_minimizer_count
            && gpu_index_verified_emit_count
                == gpu_index_retained_minimizer_count
            && gpu_index_count_emit_parity_verified == builds
            && gpu_index_target_minimizer_capacity_estimate > 0u
            && gpu_index_capacity_estimate_underestimated
                == (gpu_index_retained_minimizer_count
                        > gpu_index_target_minimizer_capacity_estimate
                    ? 1u : 0u)
            && gpu_index_arena_trimmed_before_permit_shrink == builds
            && (gpu_index_gate_enabled == 1u
                ? gpu_index_gate_count_dispatches == 1u
                    && gpu_index_gated_emit_dispatches
                        == gpu_index_deterministic_tile_emit_dispatches
                    && gpu_index_ungated_emit_dispatches == 0u
                : gpu_index_gate_enabled == 0u
                    && gpu_index_gate_count_dispatches == 0u
                    && gpu_index_gated_emit_dispatches == 0u
                    && gpu_index_ungated_emit_dispatches
                        == gpu_index_deterministic_tile_emit_dispatches);
    }

    [[nodiscard]] constexpr bool exact_stage_mask_accepted(
        bool expect_query_sketch,
        bool expect_target_sketch,
        bool expect_target_index_build,
        bool expect_seed_lookup,
        bool expect_chain_score) const noexcept {
        const auto matches = [](std::uint64_t count, bool expected) noexcept {
            return expected ? count > 0 : count == 0;
        };
        return !saturated
            && sketch_dispatches == 0
            && exact_target_index_route_accepted()
            && matches(query_sketch_dispatches, expect_query_sketch)
            && matches(target_sketch_dispatches, expect_target_sketch)
            && matches(target_index_build_dispatches,
                       expect_target_index_build)
            && matches(seed_lookup_dispatches, expect_seed_lookup)
            && matches(chain_score_dispatches, expect_chain_score);
    }

    [[nodiscard]] constexpr bool exact_sketch_token_accepted() const noexcept {
        if (sketch_dispatches != 1) {
            return false;
        }
        PartialVulkanExecutionProof expected{};
        expected.sketch_dispatches = 1;
        return *this == expected;
    }

    constexpr bool operator==(
        const PartialVulkanExecutionProof&) const noexcept = default;
};

/// A batch constructs one Phase-C target index and shares it with every query.
/// Query zero is the sole owner of the build/ownership proof so additive batch
/// aggregation cannot turn one target build into N apparent builds.
[[nodiscard]] constexpr bool phase_c_target_build_proof_owner_query(
    std::size_t query_index) noexcept
{
    return query_index == 0u;
}

static_assert([] {
    PartialVulkanExecutionProof proof{};
    proof.chain_score_dispatches = 1;
    return proof.exact_stage_mask_accepted(
        false, false, false, false, true);
}());
static_assert([] {
    PartialVulkanExecutionProof proof{};
    proof.query_sketch_dispatches = 1;
    proof.target_sketch_dispatches = 1;
    proof.target_index_build_dispatches = 1;
    proof.gpu_index_exact_count_dispatches = 1;
    proof.gpu_index_exact_count_prefix_scan_dispatches = 3;
    proof.gpu_index_exact_count_control_readbacks = 1;
    proof.gpu_index_deterministic_tile_emit_dispatches = 1;
    proof.gpu_index_emit_validation_control_readbacks = 1;
    proof.gpu_index_counted_minimizer_count = 53;
    proof.gpu_index_verified_emit_count = 53;
    proof.gpu_index_count_emit_parity_verified = 1;
    proof.gpu_index_target_minimizer_capacity_estimate = 100;
    proof.gpu_index_arena_trimmed_before_permit_shrink = 1;
    proof.gpu_index_ungated_emit_dispatches = 1;
    proof.gpu_index_retained_minimizer_count = 53;
    proof.seed_lookup_dispatches = 1;
    proof.chain_score_dispatches = 1;
    return proof.exact_stage_mask_accepted(true, true, true, true, true)
        && !proof.exact_stage_mask_accepted(true, true, false, true, true);
}());
static_assert([] {
    PartialVulkanExecutionProof lhs{};
    lhs.gpu_index_materializations = 1;
    lhs.gpu_index_open_addressed_materializations = 1;
    lhs.gpu_index_unique_keys = 3;
    lhs.gpu_index_max_probe = 7;
    PartialVulkanExecutionProof rhs{};
    rhs.gpu_index_materializations = 1;
    rhs.gpu_index_open_addressed_materializations = 1;
    rhs.gpu_index_unique_keys = 5;
    rhs.gpu_index_max_probe = 4;
    lhs.combine(rhs);
    return lhs.gpu_index_materializations == 2
        && lhs.gpu_index_open_addressed_materializations == 2
        && lhs.gpu_index_unique_keys == 8
        && lhs.gpu_index_max_probe == 7;
}());

/**
 * @brief Owning container of alignment results with PAF output formatting.
 *
 * Wraps a std::vector<MatchResult> and provides a print() method that outputs
 * results in exact minimap2 PAF format, reproducing mm_write_paf4()
 * (format.c:427-460) with auxiliary tags from write_tags() (format.c:399-425).
 */
struct MatchResultCollection {
    std::vector<MatchResult> results;
    TwoStackExecutionProof exact_simd_query_execution_proof{};
    TwoStackExecutionProof exact_simd_target_execution_proof{};
    PackedPaperNthashExecutionProof paper_nthash_query_execution_proof{};
    PackedPaperNthashExecutionProof paper_nthash_target_execution_proof{};
    SimdChainingExecutionProof simd_chaining_execution_proof{};
    BlockAlignerExecutionProof block_aligner_execution_proof{};
    ScalarPipelineExecutionProof scalar_pipeline_execution_proof{};
    Ksw2BatchedExecutionProof ksw2_batched_execution_proof{};
    SneakySnakePrefilterExecutionProof
        sneaky_snake_prefilter_execution_proof{};
    PartialVulkanExecutionProof partial_vulkan_execution_proof{};
    bioseq::indexing::PreparedFragmentAnchorBatchEvidence
        fragment_native_query_evidence{};
    bool exact_simd_query_execution_proof_present = false;
    bool exact_simd_target_execution_proof_present = false;
    bool paper_nthash_query_execution_proof_present = false;
    bool paper_nthash_target_execution_proof_present = false;
    bool simd_chaining_execution_proof_present = false;
    bool block_aligner_execution_proof_present = false;
    bool scalar_pipeline_execution_proof_present = false;
    bool ksw2_batched_execution_proof_present = false;
    bool sneaky_snake_prefilter_execution_proof_present = false;
    bool fragment_native_query_evidence_present = false;
    // Operation-local CPU/SIMD wall clocks. These travel with the result so
    // parallel rows never update shared statement statistics. The PostgreSQL
    // dispatch boundary consumes them exactly once into the existing stable
    // PipelineStage timing slots.
    std::uint64_t query_sketch_host_wall_ns = 0;
    std::uint64_t persisted_index_decode_host_wall_ns = 0;
    std::uint64_t seed_lookup_host_wall_ns = 0;
    std::uint64_t chain_score_host_wall_ns = 0;
    std::uint64_t region_generation_host_wall_ns = 0;
    std::uint64_t alignment_result_host_wall_ns = 0;
    bool host_stage_timing_saturated = false;
    // Coarse clocks cross stage objects but never escape a live pipeline.
    // The boundary methods below turn them into completed, mergeable deltas
    // before the collection is returned to its caller.
    std::uint64_t region_host_phase_started_ns = 0;
    std::uint64_t alignment_result_host_phase_started_ns = 0;
    bool region_host_phase_active = false;
    bool alignment_result_host_phase_active = false;

    MatchResultCollection() = default;
    explicit MatchResultCollection(std::vector<MatchResult>&& results_vec)
        : results(std::move(results_vec)) {}

    void record_exact_simd_query_execution_proof(
            const TwoStackExecutionProof& proof) noexcept {
        exact_simd_query_execution_proof.combine(proof);
        exact_simd_query_execution_proof_present = true;
    }

    void record_exact_simd_target_execution_proof(
            const TwoStackExecutionProof& proof) noexcept {
        exact_simd_target_execution_proof.combine(proof);
        exact_simd_target_execution_proof_present = true;
    }

    void record_paper_nthash_query_execution_proof(
            const PackedPaperNthashExecutionProof& proof) noexcept {
        paper_nthash_query_execution_proof.combine(proof);
        paper_nthash_query_execution_proof_present = true;
    }

    void record_paper_nthash_target_execution_proof(
            const PackedPaperNthashExecutionProof& proof) noexcept {
        paper_nthash_target_execution_proof.combine(proof);
        paper_nthash_target_execution_proof_present = true;
    }

    void record_simd_chaining_execution_proof(
            const SimdChainingExecutionProof& proof) noexcept {
        simd_chaining_execution_proof.combine(proof);
        simd_chaining_execution_proof_present = true;
    }

    void record_block_aligner_execution_proof(
            const BlockAlignerExecutionProof& proof) noexcept {
        block_aligner_execution_proof.combine(proof);
        block_aligner_execution_proof_present = true;
    }

    void record_scalar_pipeline_execution_proof(
            const ScalarPipelineExecutionProof& proof) noexcept {
        scalar_pipeline_execution_proof.combine(proof);
        scalar_pipeline_execution_proof_present = true;
    }

    void record_ksw2_batched_execution_proof(
            const Ksw2BatchedExecutionProof& proof) noexcept {
        ksw2_batched_execution_proof.combine(proof);
        ksw2_batched_execution_proof_present = true;
    }

    void record_sneaky_snake_prefilter_execution_proof(
            const SneakySnakePrefilterExecutionProof& proof) noexcept {
        sneaky_snake_prefilter_execution_proof.combine(proof);
        sneaky_snake_prefilter_execution_proof_present = true;
    }

    void record_scalar_query_minimizer_execution_proof(
            const ScalarMinimizerExecutionProof& proof) noexcept {
        ScalarPipelineExecutionProof routed{};
        routed.query_minimizers = proof;
        record_scalar_pipeline_execution_proof(routed);
    }

    void record_scalar_target_minimizer_execution_proof(
            const ScalarMinimizerExecutionProof& proof) noexcept {
        ScalarPipelineExecutionProof routed{};
        routed.target_minimizers = proof;
        record_scalar_pipeline_execution_proof(routed);
    }

    void record_partial_vulkan_execution_proof(
            const PartialVulkanExecutionProof& proof) noexcept {
        partial_vulkan_execution_proof.combine(proof);
    }

    void record_fragment_native_query_evidence(
            const bioseq::indexing::PreparedFragmentAnchorBatchEvidence&
                evidence) noexcept {
        fragment_native_query_evidence.merge(evidence);
        fragment_native_query_evidence_present = true;
    }

    void record_host_stage_time(
            std::uint64_t& destination,
            const std::uint64_t elapsed_ns) noexcept {
        if (elapsed_ns
                > (std::numeric_limits<std::uint64_t>::max)() - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            host_stage_timing_saturated = true;
            return;
        }
        destination += elapsed_ns;
    }

    void record_query_sketch_host_time(
            const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(query_sketch_host_wall_ns, elapsed_ns);
    }

    void record_persisted_index_decode_host_time(
            const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(
            persisted_index_decode_host_wall_ns, elapsed_ns);
    }

    void record_seed_lookup_host_time(const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(seed_lookup_host_wall_ns, elapsed_ns);
    }

    void record_chain_score_host_time(const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(chain_score_host_wall_ns, elapsed_ns);
    }

    void record_region_generation_host_time(
            const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(region_generation_host_wall_ns, elapsed_ns);
    }

    void record_alignment_result_host_time(
            const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(alignment_result_host_wall_ns, elapsed_ns);
    }

    /// Start the region-generation/selection owner after chaining completes.
    void begin_region_host_phase(const std::uint64_t started_ns) noexcept {
        if (region_host_phase_active || alignment_result_host_phase_active) {
            host_stage_timing_saturated = true;
        }
        region_host_phase_started_ns = started_ns;
        region_host_phase_active = true;
    }

    /// Close regions and, when alignment is enabled, reuse the exact same
    /// boundary as the start of the inclusive alignment/result owner.
    void transition_region_to_alignment_result_host_phase(
            const std::uint64_t boundary_ns,
            const bool alignment_enabled) noexcept {
        if (!region_host_phase_active
            || boundary_ns < region_host_phase_started_ns
            || alignment_result_host_phase_active) {
            host_stage_timing_saturated = true;
            region_host_phase_active = false;
            alignment_result_host_phase_active = false;
            return;
        }
        record_region_generation_host_time(
            std::max<std::uint64_t>(
                1u, boundary_ns - region_host_phase_started_ns));
        region_host_phase_active = false;
        if (alignment_enabled) {
            alignment_result_host_phase_started_ns = boundary_ns;
            alignment_result_host_phase_active = true;
        }
    }

    /// Close the inclusive alignment, post-alignment filters, MAPQ and result
    /// materialisation owner at the terminal sink.
    void finish_alignment_result_host_phase(
            const std::uint64_t finished_ns) noexcept {
        if (!alignment_result_host_phase_active) return;
        if (finished_ns < alignment_result_host_phase_started_ns) {
            host_stage_timing_saturated = true;
            alignment_result_host_phase_active = false;
            return;
        }
        record_alignment_result_host_time(
            std::max<std::uint64_t>(
                1u,
                finished_ns - alignment_result_host_phase_started_ns));
        alignment_result_host_phase_active = false;
    }

    void combine_execution_proof(
            const MatchResultCollection& other) noexcept {
        if (other.exact_simd_query_execution_proof_present) {
            record_exact_simd_query_execution_proof(
                other.exact_simd_query_execution_proof);
        }
        if (other.exact_simd_target_execution_proof_present) {
            record_exact_simd_target_execution_proof(
                other.exact_simd_target_execution_proof);
        }
        if (other.paper_nthash_query_execution_proof_present) {
            record_paper_nthash_query_execution_proof(
                other.paper_nthash_query_execution_proof);
        }
        if (other.paper_nthash_target_execution_proof_present) {
            record_paper_nthash_target_execution_proof(
                other.paper_nthash_target_execution_proof);
        }
        if (other.simd_chaining_execution_proof_present) {
            record_simd_chaining_execution_proof(
                other.simd_chaining_execution_proof);
        }
        if (other.block_aligner_execution_proof_present) {
            record_block_aligner_execution_proof(
                other.block_aligner_execution_proof);
        }
        if (other.scalar_pipeline_execution_proof_present) {
            record_scalar_pipeline_execution_proof(
                other.scalar_pipeline_execution_proof);
        }
        if (other.ksw2_batched_execution_proof_present) {
            record_ksw2_batched_execution_proof(
                other.ksw2_batched_execution_proof);
        }
        if (other.sneaky_snake_prefilter_execution_proof_present) {
            record_sneaky_snake_prefilter_execution_proof(
                other.sneaky_snake_prefilter_execution_proof);
        }
        partial_vulkan_execution_proof.combine(
            other.partial_vulkan_execution_proof);
        if (other.fragment_native_query_evidence_present) {
            record_fragment_native_query_evidence(
                other.fragment_native_query_evidence);
        }
        record_query_sketch_host_time(other.query_sketch_host_wall_ns);
        record_persisted_index_decode_host_time(
            other.persisted_index_decode_host_wall_ns);
        record_seed_lookup_host_time(other.seed_lookup_host_wall_ns);
        record_chain_score_host_time(other.chain_score_host_wall_ns);
        record_region_generation_host_time(
            other.region_generation_host_wall_ns);
        record_alignment_result_host_time(
            other.alignment_result_host_wall_ns);
        host_stage_timing_saturated = host_stage_timing_saturated
            || other.host_stage_timing_saturated
            || region_host_phase_active
            || alignment_result_host_phase_active
            || other.region_host_phase_active
            || other.alignment_result_host_phase_active;
    }

    [[nodiscard]] bool exact_simd_query_execution_accepted(
            bool require_nonempty_vector_work) const noexcept {
        return exact_simd_query_execution_proof_present
            && exact_simd_query_execution_proof.accepted(
                require_nonempty_vector_work);
    }

    [[nodiscard]] bool exact_simd_target_execution_accepted(
            bool require_nonempty_vector_work) const noexcept {
        return exact_simd_target_execution_proof_present
            && exact_simd_target_execution_proof.accepted(
                require_nonempty_vector_work);
    }

    [[nodiscard]] bool paper_nthash_query_execution_accepted(
            bool require_nonempty_vector_work) const noexcept {
        return paper_nthash_query_execution_proof_present
            && paper_nthash_query_execution_proof.accepted(
                require_nonempty_vector_work);
    }

    [[nodiscard]] bool paper_nthash_target_execution_accepted(
            bool require_nonempty_vector_work) const noexcept {
        return paper_nthash_target_execution_proof_present
            && paper_nthash_target_execution_proof.accepted(
                require_nonempty_vector_work);
    }

    [[nodiscard]] bool simd_chaining_execution_accepted(
            SimdChainingPolicy selected_policy,
            bool require_nonempty_vector_work) const noexcept {
        return simd_chaining_execution_proof_present
            && simd_chaining_execution_proof.accepted(
                selected_policy, require_nonempty_vector_work);
    }

    /// Compatibility spelling for callers that intentionally exercise the
    /// exact C++ default.  Production/benchmark acceptance must pass the
    /// requested policy explicitly so observed bounded evidence cannot be
    /// mislabelled as exact (or vice versa).
    [[nodiscard]] bool simd_chaining_execution_accepted(
            bool require_nonempty_vector_work) const noexcept {
        return simd_chaining_execution_accepted(
            SimdChainingPolicy::exact_visited,
            require_nonempty_vector_work);
    }

    [[nodiscard]] bool ksw2_batched_execution_accepted(
            const bool require_nonempty_work) const noexcept {
        return ksw2_batched_execution_proof_present
            && ksw2_batched_execution_proof.accepted(
                require_nonempty_work);
    }

    [[nodiscard]] bool selected_simd_sketch_execution_accepted(
            MinimizerAlgorithmType selected,
            bool require_target_sketch,
            bool require_nonempty_vector_work) const noexcept {
        if (partial_vulkan_execution_proof
                != PartialVulkanExecutionProof{}) {
            return false;
        }

        const bool exact_query_absent =
            !exact_simd_query_execution_proof_present
            && exact_simd_query_execution_proof == TwoStackExecutionProof{};
        const bool exact_target_absent =
            !exact_simd_target_execution_proof_present
            && exact_simd_target_execution_proof == TwoStackExecutionProof{};
        const bool paper_query_absent =
            !paper_nthash_query_execution_proof_present
            && paper_nthash_query_execution_proof
                == PackedPaperNthashExecutionProof{};
        const bool paper_target_absent =
            !paper_nthash_target_execution_proof_present
            && paper_nthash_target_execution_proof
                == PackedPaperNthashExecutionProof{};

        switch (selected) {
            case MinimizerAlgorithmType::TWO_STACK_NTHASH:
                return paper_query_absent
                    && paper_target_absent
                    && exact_simd_query_execution_accepted(
                        require_nonempty_vector_work)
                    && (require_target_sketch
                        ? exact_simd_target_execution_accepted(
                            require_nonempty_vector_work)
                        : exact_target_absent);
            case MinimizerAlgorithmType::PAPER_NTHASH_SIMD:
                return exact_query_absent
                    && exact_target_absent
                    && paper_nthash_query_execution_accepted(
                        require_nonempty_vector_work)
                    && (require_target_sketch
                        ? paper_nthash_target_execution_accepted(
                            require_nonempty_vector_work)
                        : paper_target_absent);
            default:
                return false;
        }
    }

    /// Validate the SIMD sketch/chaining/Block tuple plus its real CPU seed
    /// lookup. All other scalar biological dimensions remain forbidden.
    [[nodiscard]] bool all_simd_execution_accepted(
            MinimizerAlgorithmType selected,
            SimdChainingPolicy selected_chaining_policy,
            bool require_target_sketch,
            bool require_nonempty_vector_work) const noexcept {
        const auto& scalar = scalar_pipeline_execution_proof;
        const bool scalar_seed_only_accepted =
            scalar_pipeline_execution_proof_present
            && !scalar.saturated
            && scalar.query_minimizers == ScalarMinimizerExecutionProof{}
            && scalar.target_minimizers == ScalarMinimizerExecutionProof{}
            && scalar.seed_lookup_calls > 0u
            && (!require_nonempty_vector_work
                || scalar.seed_query_minimizers > 0u)
            && scalar.chain_score_calls == 0u
            && scalar.chain_backtrack_calls == 0u
            && scalar.chain_anchor_inputs == 0u
            && scalar.ksw2_left_calls == 0u
            && scalar.ksw2_gap_calls == 0u
            && scalar.ksw2_right_calls == 0u
            && scalar.ksw2_input_cell_area == 0u;
        return scalar_seed_only_accepted
            && !ksw2_batched_execution_proof_present
            && ksw2_batched_execution_proof
                == Ksw2BatchedExecutionProof{}
            && selected_simd_sketch_execution_accepted(
                selected,
                require_target_sketch,
                require_nonempty_vector_work)
            && simd_chaining_execution_accepted(
                selected_chaining_policy,
                require_nonempty_vector_work)
            && block_aligner_execution_accepted(
                require_nonempty_vector_work);
    }

    [[nodiscard]] bool all_simd_execution_accepted(
            MinimizerAlgorithmType selected,
            bool require_target_sketch,
            bool require_nonempty_vector_work) const noexcept {
        return all_simd_execution_accepted(
            selected,
            SimdChainingPolicy::exact_visited,
            require_target_sketch,
            require_nonempty_vector_work);
    }

    [[nodiscard]] bool all_scalar_execution_accepted(
            const bool require_target_sketch,
            const bool require_alignment) const noexcept {
        const bool simd_chain_absent =
            !simd_chaining_execution_proof_present
            && simd_chaining_execution_proof
                == SimdChainingExecutionProof{};
        const bool block_absent =
            !block_aligner_execution_proof_present
            && block_aligner_execution_proof
                == BlockAlignerExecutionProof{};
        const bool batched_absent =
            !ksw2_batched_execution_proof_present
            && ksw2_batched_execution_proof
                == Ksw2BatchedExecutionProof{};
        return selected_scalar_sketch_execution_accepted(
                require_target_sketch,
                /*require_nonempty_work=*/true)
            && scalar_pipeline_execution_proof.accepted(
                require_target_sketch, require_alignment)
            && simd_chain_absent
            && block_absent
            && batched_absent
            && partial_vulkan_execution_proof
                == PartialVulkanExecutionProof{};
    }

    [[nodiscard]] bool selected_scalar_sketch_execution_accepted(
            const bool require_target_sketch,
            const bool require_nonempty_work) const noexcept {
        const bool exact_absent =
            !exact_simd_query_execution_proof_present
            && !exact_simd_target_execution_proof_present
            && exact_simd_query_execution_proof == TwoStackExecutionProof{}
            && exact_simd_target_execution_proof == TwoStackExecutionProof{};
        const bool paper_absent =
            !paper_nthash_query_execution_proof_present
            && !paper_nthash_target_execution_proof_present
            && paper_nthash_query_execution_proof
                == PackedPaperNthashExecutionProof{}
            && paper_nthash_target_execution_proof
                == PackedPaperNthashExecutionProof{};
        const bool target_valid = require_target_sketch
            ? scalar_pipeline_execution_proof.target_minimizers.accepted(
                require_nonempty_work)
            : scalar_pipeline_execution_proof.target_minimizers
                == ScalarMinimizerExecutionProof{};
        return scalar_pipeline_execution_proof_present
            && scalar_pipeline_execution_proof.query_minimizers.accepted(
                require_nonempty_work)
            && target_valid
            && exact_absent
            && paper_absent
            && partial_vulkan_execution_proof
                == PartialVulkanExecutionProof{};
    }

    [[nodiscard]] bool has_any_algorithm_execution_proof() const noexcept {
        return exact_simd_query_execution_proof_present
            || exact_simd_target_execution_proof_present
            || paper_nthash_query_execution_proof_present
            || paper_nthash_target_execution_proof_present
            || simd_chaining_execution_proof_present
            || block_aligner_execution_proof_present
            || scalar_pipeline_execution_proof_present
            || ksw2_batched_execution_proof_present
            || partial_vulkan_execution_proof.has_any_dispatch();
    }

    [[nodiscard]] bool block_aligner_execution_accepted(
            bool require_nonempty_simd_work) const noexcept {
        if (!block_aligner_execution_proof_present
            || !block_aligner_execution_proof.accepted()) {
            return false;
        }
        if (!require_nonempty_simd_work) {
            return true;
        }
        const auto& proof = block_aligner_execution_proof;
        return proof.executed_calls() > 0
            && proof.nonempty_calls > 0
            && proof.nonempty_calls_with_simd == proof.nonempty_calls
            && ((proof.simd_dp_vector_batches > 0
                    && proof.simd_dp_cells > 0)
                || proof.simd_diagonal_certificate_calls > 0);
    }

    [[nodiscard]] size_t size() const { return results.size(); }
    [[nodiscard]] bool empty() const { return results.empty(); }

    [[nodiscard]] std::span<const MatchResult> view() const {
        return std::span<const MatchResult>(results);
    }

    /**
     * @brief Print alignment results in exact minimap2 PAF format.
     *
     * PAF line format (tab-separated):
     *   col 1:  query name             -- user-provided
     *   col 2:  query sequence length
     *   col 3:  query start (0-based, closed)
     *   col 4:  query end   (0-based, open)
     *   col 5:  relative strand '+' or '-'
     *   col 6:  target name            -- user-provided
     *   col 7:  target sequence length
     *   col 8:  target start on original strand (0-based)
     *   col 9:  target end   on original strand (0-based)
     *   col 10: number of residue matches  (match_length)
     *   col 11: alignment block length     (block_length)
     *   col 12: mapping quality (0-255)
     *
     * Auxiliary tags: NM, ms, AS, nn, tp, cm, s1, s2, de/dv, rl, cg
     *
     * @param query_name  Query sequence name (col 1)
     * @param target_name Target sequence name (col 6)
     */
    void print(const std::string& query_name  = "query",
               const std::string& target_name = "target") const
    {
        if (results.empty()) {
            bioseq_log<DBG>("{}\t0\t0\t0\t*\t*\t0\t0\t0\t0\t0\t0", query_name);
            return;
        }

        for (const auto& match : results) {
            const char strand_char = match.is_reverse ? '-' : '+';
            const char type_char = match.is_primary ? 'P' : 'S';

            std::string line = fmt::format(
                "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                query_name, match.query_len, match.query_start, match.query_end,
                strand_char, target_name, match.target_len, match.target_start,
                match.target_end, match.num_matches, match.block_length, match.mapq);

            if (match.has_cigar && !match.cigar.empty()) {
                // NM = blen - mlen; both are sequence_size_t (uint64) and
                // blen >= mlen, but compute signed + clamp defensively.
                const std::int64_t edit_distance = std::max<std::int64_t>(0,
                    static_cast<std::int64_t>(match.block_length)
                  - static_cast<std::int64_t>(match.num_matches));
                line += fmt::format("\tNM:i:{}\tms:i:{}\tAS:i:{}\tnn:i:0",
                    edit_distance, match.dp_max_score, match.dp_score);
            }

            line += fmt::format("\ttp:A:{}", type_char);
            line += fmt::format("\tcm:i:{}", match.seed_count);
            line += fmt::format("\ts1:i:{}", match.chain_score);

            if (match.is_primary) {
                line += fmt::format("\ts2:i:{}", match.secondary_chain_score);
            }

            if (match.divergence >= 0.0f && match.divergence <= 1.0f) {
                const char* divergence_tag = (match.has_cigar && !match.cigar.empty())
                    ? "\tde:f:" : "\tdv:f:";
                if (match.divergence == 0.0f) {
                    line += divergence_tag;
                    line += '0';
                } else {
                    line += fmt::format("{}{:.4f}", divergence_tag,
                        static_cast<double>(match.divergence));
                }
            }

            line += "\trl:i:0";

            if (match.has_cigar && !match.cigar.empty()) {
                line += "\tcg:Z:";
                line += format_cigar(match.cigar);
            }

            bioseq_log<DBG>("{}", line);
        }
    }
};

/**
 * @brief Print alignment results in exact minimap2 PAF format.
 *
 * Free function wrapper for backwards compatibility. Delegates to
 * MatchResultCollection::print().
 */
void print_match_results(
    const MatchResultCollection& collection,
    const std::string& query_name  = "query",
    const std::string& target_name = "target")
{
    collection.print(query_name, target_name);
}


} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

} // export

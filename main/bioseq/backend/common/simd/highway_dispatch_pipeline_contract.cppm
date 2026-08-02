/**
 * @file highway_dispatch_pipeline_contract.cppm
 * @brief Neutral scheduler and alignment ABI owned by the Highway dispatch module.
 */
module;

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

export module bioseq.highway.dispatch_contract:pipeline;

export import bioseq.alignment.contracts;
export import bioseq.sequences.cigar;
export import bioseq.sequences.chaining.utils;
export import bioseq.storage.lazy;
export import bioseq.storage.size;
import bioseq.sequences;

export namespace bioseq::simd_dispatch {

struct HighwayKernelTable;

struct HighwayDispatchInfo final {
    std::int64_t compiled_targets = 0;
    std::int64_t cpu_supported_targets = 0;
    std::int64_t runnable_targets = 0;
    std::int64_t planned_target = 0;
};

static_assert(std::is_standard_layout_v<HighwayDispatchInfo>);
static_assert(std::is_trivially_copyable_v<HighwayDispatchInfo>);

}  // namespace bioseq::simd_dispatch

export namespace bioseq::prefilter_stats {

enum class PrefilterBackendType : std::uint8_t {
    AUTO    = 0,
    SCALAR  = 1,
    SIMD    = 2,
    VULKAN  = 3,
};

}  // namespace bioseq::prefilter_stats

export namespace bioseq::simd {

using bioseq::chaining::ChainingMode;
using bioseq::chaining::ChainingParams;
using bioseq::chaining::SimdChainingPolicy;
using bioseq::storage::sequence_size_t;

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

struct Minimap2Params {
    /// Immutable target binding selected exactly once by BatchProcessor.
    /// Neutral pipeline code carries these values; leaves never query Highway.
    const ::bioseq::simd_dispatch::HighwayKernelTable* highway_kernels = nullptr;
    ::bioseq::simd_dispatch::HighwayDispatchInfo highway_dispatch{};
    std::int64_t highway_executed_target = 0;
    /// Central scheduler-registry code fixed by the outer alphabet dispatch.
    std::uint8_t alphabet_code = 0xffu;

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
    ::bioseq::prefilter_stats::PrefilterBackendType prefilter_backend =
        ::bioseq::prefilter_stats::PrefilterBackendType::AUTO;

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

/** Exact fixed-target alignment functions for one logical alphabet. */
template<sequence_alphabet Alphabet>
struct AlignmentKernels final {
    using View = bioseq::storage::basic_lazy_sequence_view<Alphabet>;
    using Matrix = ScoringMatrix<Alphabet>;

    using KswZ2 = KswAlignResult (*)(
        bool, View, View, std::size_t, std::size_t, std::size_t, std::size_t,
        const Matrix&, const KswZ2Params&);
    using KswD2 = KswAlignResult (*)(
        bool, View, View, std::size_t, std::size_t, std::size_t, std::size_t,
        const Matrix&, KswD2Params);
    using KswBatched = void (*)(
        std::span<AlignRegion>,
        std::span<const bioseq::chaining::Anchor>,
        View, View, const Matrix&, const Minimap2Params&,
        Ksw2BatchedExecutionProof*);
    using PrepareBlock = BlockGlobalGapCertificatePlan (*)(
        const Matrix&, const BlockAlignerParams&) noexcept;
    using TryBlockGap = BlockPackedCertificateAttempt (*)(
        bool, View, View, std::size_t, std::size_t, std::size_t, std::size_t,
        const BlockGlobalGapCertificatePlan&) noexcept;
    using BlockAlign = KswAlignResult (*)(
        bool, bool, View, View,
        std::size_t, std::size_t, std::size_t, std::size_t,
        const Matrix&, const BlockAlignerParams&,
        BlockAlignerExecutionProof*, BlockAlignerConstructionEvidence*);

    KswZ2 ksw2_z2 = nullptr;
    KswD2 ksw2_d2 = nullptr;
    KswBatched ksw2_batched = nullptr;
    PrepareBlock block_prepare = nullptr;
    TryBlockGap block_try_certified_gap = nullptr;
    BlockAlign block_align = nullptr;
};

template<sequence_alphabet Alphabet>
inline constexpr bool alignment_kernels_are_neutral =
    std::is_standard_layout_v<AlignmentKernels<Alphabet>>
    && std::is_trivially_copyable_v<AlignmentKernels<Alphabet>>;

struct AlignmentTableEntry final {
    std::uint8_t alphabet_code = 0;
    const void* typed_table = nullptr;
};

static_assert(std::is_standard_layout_v<AlignmentTableEntry>);
static_assert(std::is_trivially_copyable_v<AlignmentTableEntry>);

}  // namespace bioseq::simd

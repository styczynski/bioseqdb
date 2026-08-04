/**
 * @file runtime_contract.cppm
 * @brief Target-neutral runtime algorithm and dispatch identities.
 */
module;

#include <cstdint>
#include <type_traits>

export module bioseq.pipeline.runtime_contract;

export namespace bioseq::simd_dispatch {

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

}  // namespace bioseq::simd

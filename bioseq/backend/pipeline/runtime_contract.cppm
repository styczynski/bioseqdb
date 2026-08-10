/**
 * @file runtime_contract.cppm
 * @brief Target-neutral runtime algorithm and dispatch identities.
 */
module;

#include <cstdint>
#include <type_traits>

export module bioseq.pipeline.runtime_contract;

export namespace bioseq::simd_dispatch {

/// Target-neutral masks selected by the single registered Highway dispatcher.
struct highway_dispatch_plan final {
    std::int64_t compiled_targets = 0;
    std::int64_t cpu_supported_targets = 0;
    std::int64_t runnable_targets = 0;
    std::int64_t planned_target = 0;
};

static_assert(std::is_standard_layout_v<highway_dispatch_plan>);
static_assert(std::is_trivially_copyable_v<highway_dispatch_plan>);

}  // namespace bioseq::simd_dispatch

export namespace bioseq::prefilter {

/// Backend that executed a completed prefilter batch.
enum class prefilter_backend : std::uint8_t {
    automatic = 0,
    scalar = 1,
    simd = 2,
    vulkan = 3,
};

}  // namespace bioseq::prefilter

export namespace bioseq::simd {

/// Seed-chaining implementation selected for one alignment operation.
enum class chaining_algorithm : std::uint8_t {
    scalar_minimap2 = 0,
    simd_minimap2 = 1,
    range_minimum_query = 2,
    /// Tiled dynamic-programming score generation on Vulkan.
    vulkan_tiled_dynamic_programming = 3,
};

/// Base-level alignment algorithm — KSW2 dual-affine DP (minimap2 default)
/// vs Highway-vectorised block aligner (single-affine). Values MUST match
/// bioseq.aligner.base_alignment.algorithm ordinals.
enum class base_alignment_algorithm : std::uint8_t {
    ksw2 = 0,
    block_aligner = 1,
    /// Inter-region batched KSW2. Packs many small KSW2
    /// gap-fills across regions into SIMD lanes and runs an inter-sequence
    /// DP. Long fills (q_max > 256 || t_max > 256) fall through to the
    /// intra-sequence striped KSW2 kernel via the same trait surface.
    ksw2_batched = 2,
};

/// Minimizer-extraction algorithm. Selected at runtime; the
/// `minimap_port` entry point dispatches on this enum with a runtime
/// branch (cannot use constexpr-if because both paths instantiate the
/// same templates, just with the simd_hash vs scalar_hash inner type,
/// and the kmer index module's `requires` clauses already enforce the
/// correct concept).
enum class minimizer_algorithm : std::uint8_t {
    /// Scalar minimizer collector.
    scalar = 0,
    /// Highway-vectorised minimap2-compatible reference collector.
    /// Its running-min rescan is retained as a correctness oracle and has
    /// been measured to be slower than the scalar baseline on the
    /// long-input cost because any lane can invalidate the saved minimum.
    simd_minimap2_reference = 1,
    /// Production minimizer collector. Two-stack / prefix-suffix-min
    /// sliding-window with dual-stream interleaved per-step body
    /// (mirrors the SIMD-Minimizers paper) and ntHash-style mix +
    /// deferred minimap-integer scramble.
    two_stack_nthash = 2,
    /// Vulkan minimap-integer minimizer path. Requires a
    /// usable Vulkan runtime when selected by the pipeline.
    vulkan_minimap_integer = 3,
    /// Bounded-memory SIMD implementation of the u32 seq-hash ntHash scheme
    /// used by the SIMD-Minimizers work: R=7 rolling hashes, wrapping
    /// forward+reverse canonical sum, and an eight-logical-stream two-stack
    /// window. This has a distinct persisted scheme identity from the
    /// minimap-int/ExactSimd paths and must never consume their indexes.
    bounded_nthash_simd = 4,
};

}  // namespace bioseq::simd

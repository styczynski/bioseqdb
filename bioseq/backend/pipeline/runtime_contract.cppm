/**
 * @file runtime_contract.cppm
 * @brief Target-neutral runtime algorithm and dispatch identities.
 */
module;

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

export module bioseq.pipeline.runtime_contract;

export namespace bioseq::simd_dispatch {

/**
 * @brief Kernel families bound by one fixed Highway target.
 *
 * Each enumerator owns one bit in highway_kernel_mask. The `count` sentinel is
 * excluded from the mask and is never a runnable kernel.
 */
enum class highway_kernel : std::uint8_t {
    /** Minimap2 reference minimizer extraction. */
    minimizer_minimap2_reference,
    /** Two-stack ntHash minimizer extraction. */
    minimizer_two_stack_nthash,
    /** Bounded-window ntHash minimizer extraction. */
    minimizer_bounded_nthash,
    /** SIMD dynamic-programming anchor chaining. */
    chaining_chain_anchors_simd,
    /** SIMD SneakySnake batch prefiltering. */
    prefilter_sneaky_snake_simd_batch,
    /** Single-region KSW2 alignment. */
    alignment_ksw2,
    /** Cross-region batched KSW2 alignment. */
    alignment_ksw2_batched,
    /** Block-based global alignment. */
    alignment_block_aligner,
    /** Number of kernel families. */
    count,
};

/** @brief Number of callable kernel families represented by the dispatch mask. */
inline constexpr std::size_t highway_kernel_count =
    static_cast<std::size_t>(highway_kernel::count);

/** @brief Bit mask over all callable Highway kernel families. */
using highway_kernel_mask = std::bitset<highway_kernel_count>;

/// Target-neutral masks selected by the single registered Highway dispatcher.
struct highway_dispatch_plan final {
    /// Highway targets present in the linked runtime.
    std::int64_t compiled_targets = 0;
    /// Compiled targets supported by the current CPU.
    std::int64_t cpu_supported_targets = 0;
    /// Supported targets admitted by the runtime contract.
    std::int64_t runnable_targets = 0;
    /// Single target selected for this dispatch plan.
    std::int64_t planned_target = 0;
};

static_assert(std::is_standard_layout_v<highway_dispatch_plan>);
static_assert(std::is_trivially_copyable_v<highway_dispatch_plan>);

}  // namespace bioseq::simd_dispatch

export namespace bioseq::prefilter {

/// Backend that executed a completed prefilter batch.
enum class prefilter_backend : std::uint8_t {
    /// Resolve the backend from the active execution policy.
    automatic = 0,
    /// Execute the scalar rank prefilter.
    scalar = 1,
    /// Execute the selected Highway rank prefilter.
    simd = 2,
    /// Execute the Vulkan rank prefilter.
    vulkan = 3,
};

}  // namespace bioseq::prefilter

export namespace bioseq::simd {

/// Seed-chaining implementation selected for one alignment operation.
enum class chaining_algorithm : std::uint8_t {
    /// Minimap2 dynamic programming on the scalar CPU route.
    scalar_minimap2 = 0,
    /// Minimap2 dynamic programming through the selected Highway target.
    simd_minimap2 = 1,
    /// Range-minimum-query chaining on the CPU route.
    range_minimum_query = 2,
    /// Tiled dynamic-programming score generation on Vulkan.
    vulkan_tiled_dynamic_programming = 3,
};

/// Base-level alignment algorithm selected for one alignment operation.
/// Ordinals match `bioseq.aligner.base_alignment.algorithm`.
enum class base_alignment_algorithm : std::uint8_t {
    /// KSW2 dual-affine dynamic programming.
    ksw2 = 0,
    /// Highway-vectorised single-affine block alignment.
    block_aligner = 1,
    /// Inter-region batched KSW2. Packs many small KSW2
    /// gap-fills across regions into SIMD lanes and runs an inter-sequence
    /// DP. Long fills (q_max > 256 || t_max > 256) fall through to the
    /// intra-sequence striped KSW2 kernel via the same trait surface.
    ksw2_batched = 2,
};

/// Minimizer-extraction algorithm. Selected at runtime. The
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

export namespace bioseq::simd::pipe {

/** @brief One stable name/value property rendered in pipeline descriptions. */
struct named_property {
    std::string name;
    std::string value;
};

/** @brief Algorithm identity and resolved properties for one pipeline stage. */
struct stage_algorithm_description {
    std::string name;
    std::vector<named_property> parameters;
    std::vector<named_property> computed_properties;
    std::optional<std::string> extra_description;
    ::bioseq::simd_dispatch::highway_kernel_mask highway_kernels{};
};

/** @brief Stable EXPLAIN description of one executable pipeline stage. */
struct stage_description {
    std::string stage_name;
    std::optional<stage_algorithm_description> algorithm;
    std::optional<std::string> extra_description;

    /**
     * @brief Pre-allocation upper bound the stage reserves before driving its
     * kernel ( — "reserve correctness").
     *
     * Populated by every stage that opens an `arena_slab<p_element>` of bounded size
     * (minimizer_stage, seed_hit_stage, chain_stage, region_stage). Set to
     * `std::nullopt` for stages that mutate in-place or whose output size
     * is `0` (index_stage, sink_stage, etc.). The EXPLAIN renderer surfaces
     * this as a per-stage `[reserved: N]` line so a reader can verify
     * `actual ≤ reserved` against the runtime instrumentation log.
     */
    std::optional<std::size_t> estimated_batch_size;
};

/** @brief Ordered description of the exact executable pipeline. */
struct pipeline_description {
    std::vector<stage_description> stages;
};

inline constexpr std::string_view query_sketch_stage_name = "QuerySketchStage";

/** Cold EXPLAIN-only query-sketch route. Never a dispatch-chain dimension. */
enum class query_sketch_route : std::uint8_t {
    recompute_capable,
    compatible_persisted_only,
};

}  // namespace bioseq::simd::pipe

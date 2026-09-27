/**
 * @file runtime_contract.cppm
 * @brief Shared algorithm choices and pipeline descriptions.
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
 * @brief Kernel families available through a selected Highway target.
 *
 * Each family has one bit in highway_kernel_mask. The count value sizes the
 * mask and does not identify a callable kernel.
 */
enum class highway_kernel : std::uint8_t {
    /** Minimap2 reference minimizer extraction. */
    minimizer_minimap2_reference,
    /** Exact SIMD minimizer extraction selected by two_stack_nthash. */
    minimizer_two_stack_nthash,
    /** SIMD minimizer extraction using bounded 32-bit ntHash values. */
    minimizer_bounded_nthash,
    /** SIMD dynamic-programming anchor chaining. */
    chaining_chain_anchors_simd,
    /** SIMD SneakySnake batch prefiltering. */
    prefilter_sneaky_snake_simd_batch,
    /** Single-region KSW2 alignment. */
    alignment_ksw2,
    /** KSW2 alignment with fills grouped across regions. */
    alignment_ksw2_batched,
    /** Adaptive block alignment. */
    alignment_block_aligner,
    /** Number of kernel families. */
    count,
};

/** @brief Number of kernel families in the dispatch mask. */
inline constexpr std::size_t highway_kernel_count =
    static_cast<std::size_t>(highway_kernel::count);

/** @brief One bit per callable Highway kernel family. */
using highway_kernel_mask = std::bitset<highway_kernel_count>;

/// Available Highway targets and the target selected for this execution.
struct highway_dispatch_plan final {
    /// Targets compiled into the linked runtime.
    std::int64_t compiled_targets = 0;
    /// Targets supported by the current CPU.
    std::int64_t cpu_supported_targets = 0;
    /// Targets both compiled and supported by the CPU.
    std::int64_t runnable_targets = 0;
    /// Preferred runnable target, or zero when none is available.
    std::int64_t planned_target = 0;
};

static_assert(std::is_standard_layout_v<highway_dispatch_plan>);
static_assert(std::is_trivially_copyable_v<highway_dispatch_plan>);

}  // namespace bioseq::simd_dispatch

export namespace bioseq::prefilter {

/// Requested prefilter backend, or the backend recorded after execution.
enum class prefilter_backend : std::uint8_t {
    /// Choose the backend from the current execution settings.
    automatic = 0,
    /// Run the scalar prefilter on sequence ranks.
    scalar = 1,
    /// Run the prefilter with the selected Highway target.
    simd = 2,
    /// Run the prefilter through Vulkan.
    vulkan = 3,
};

}  // namespace bioseq::prefilter

export namespace bioseq::simd {

/// Algorithm used to join matching seed positions into chains.
enum class chaining_algorithm : std::uint8_t {
    /// Scalar minimap2 dynamic programming.
    scalar_minimap2 = 0,
    /// SIMD chaining with the selected exact or bounded predecessor policy.
    simd_minimap2 = 1,
    /// CPU chaining with a tree for range-minimum queries.
    range_minimum_query = 2,
    /// Compute chaining scores and predecessors through Vulkan.
    vulkan_tiled_dynamic_programming = 3,
};

/// Algorithm used to align individual bases. Codes 0 and 1 match
/// bioseq.aligner.base_alignment.algorithm. Batched KSW2 is C++ only.
enum class base_alignment_algorithm : std::uint8_t {
    /// KSW2 alignment with two gap-penalty choices.
    ksw2 = 0,
    /// SIMD adaptive block alignment with one gap-penalty choice.
    block_aligner = 1,
    /// Group KSW2 fills from several regions by size and orientation.
    /// Each fill currently runs through the single-fill kernel before its
    /// CIGAR is joined back into the corresponding region.
    ksw2_batched = 2,
};

/// Minimizer algorithm selected for index construction or query sketching.
/// Stored indexes and query sketches must use compatible minimizer rules.
enum class minimizer_algorithm : std::uint8_t {
    /// Collect minimizers with the scalar minimap2 algorithm.
    scalar = 0,
    /// SIMD version of the minimap2 running-minimum algorithm.
    /// When any lane loses its saved minimum, the window is searched again.
    simd_minimap2_reference = 1,
    /// Exact SIMD minimizer collection. The selected resident or streaming
    /// implementation must agree with the index used by the query.
    two_stack_nthash = 2,
    /// Collect minimap-integer minimizers through Vulkan.
    /// Selecting this algorithm requires a usable Vulkan runtime.
    vulkan_minimap_integer = 3,
    /// Collect minimizers using 32-bit ntHash and eight logical streams.
    /// Scratch space is bounded by the window size. Its stored indexes use
    /// a separate scheme from the minimap-integer algorithms.
    bounded_nthash_simd = 4,
};

}  // namespace bioseq::simd

export namespace bioseq::simd::pipe {

/** @brief A name and value shown in a pipeline description. */
struct named_property {
    std::string name;
    std::string value;
};

/** @brief An algorithm name, its parameters and its computed properties. */
struct stage_algorithm_description {
    std::string name;
    std::vector<named_property> parameters;
    std::vector<named_property> computed_properties;
    std::optional<std::string> extra_description;
    ::bioseq::simd_dispatch::highway_kernel_mask highway_kernels{};
};

/** @brief Description of one pipeline stage for EXPLAIN. */
struct stage_description {
    std::string stage_name;
    std::optional<stage_algorithm_description> algorithm;
    std::optional<std::string> extra_description;

    /**
     * @brief Optional output-capacity estimate shown as [reserved: N].
     *
     * A stage supplies this when it can estimate the allocation while building
     * its description. Stages whose capacity depends on runtime input, such as
     * chaining, leave it empty. This field does not enforce an allocation limit.
     */
    std::optional<std::size_t> estimated_batch_size;
};

/** @brief Pipeline stages in execution order. */
struct pipeline_description {
    std::vector<stage_description> stages;
};

inline constexpr std::string_view query_sketch_stage_name = "QuerySketchStage";

/** Select the query-sketch description for EXPLAIN without dispatching a kernel. */
enum class query_sketch_route : std::uint8_t {
    recompute_capable,
    compatible_persisted_only,
};

}  // namespace bioseq::simd::pipe
